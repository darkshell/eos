// ----------------------------------------------------------------------
// File: Mapping.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "common/Namespace.hh"
#include "common/Mapping.hh"
#include "common/Macros.hh"
#include "common/Logging.hh"
#include "common/SecEntity.hh"
#include "common/SymKeys.hh"
/*----------------------------------------------------------------------------*/
#include "XrdSys/XrdSysDNS.hh"
#include "XrdOuc/XrdOucEnv.hh"
/*----------------------------------------------------------------------------*/
#include <pwd.h>
#include <grp.h>
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN

// Constants
const std::string Mapping::PROXY_GEOTAG = "proxy";

/*----------------------------------------------------------------------------*/
// global mapping objects
/*----------------------------------------------------------------------------*/
RWMutex Mapping::gMapMutex;
XrdSysMutex Mapping::gPhysicalIdMutex;

Mapping::UserRoleMap_t Mapping::gUserRoleVector;
Mapping::GroupRoleMap_t Mapping::gGroupRoleVector;
Mapping::VirtualUserMap_t Mapping::gVirtualUidMap;
Mapping::VirtualGroupMap_t Mapping::gVirtualGidMap;
Mapping::SudoerMap_t Mapping::gSudoerMap;
bool Mapping::gRootSquash = true;

Mapping::GeoLocationMap_t Mapping::gGeoMap;

Mapping::AllowedTidentMatches_t Mapping::gAllowedTidentMatches;

XrdSysMutex Mapping::ActiveLock;

google::dense_hash_map<std::string, time_t> Mapping::ActiveTidents;

XrdOucHash<Mapping::id_pair> Mapping::gPhysicalUidCache;
XrdOucHash<Mapping::gid_vector> Mapping::gPhysicalGidCache;

XrdSysMutex Mapping::gPhysicalNameCacheMutex;
std::map<uid_t, std::string> Mapping::gPhysicalUserNameCache;
std::map<gid_t, std::string> Mapping::gPhysicalGroupNameCache;
std::map<std::string, uid_t> Mapping::gPhysicalUserIdCache;
std::map<std::string, gid_t> Mapping::gPhysicalGroupIdCache;

Mapping::ip_cache Mapping::gIpCache(300);
/*----------------------------------------------------------------------------*/
/**
 * Initialize Google maps
 *
 */

/*----------------------------------------------------------------------------*/

void
Mapping::Init()
{
  ActiveTidents.set_empty_key("#__EMPTY__#");
  ActiveTidents.set_deleted_key("#__DELETED__#");

  // allow FUSE client access as root via env variable
  if (getenv("EOS_FUSE_NO_ROOT_SQUASH") &&
      !strcmp("1", getenv("EOS_FUSE_NO_ROOT_SQUASH"))) {
    gRootSquash = false;
  }
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void
Mapping::Reset()
{
  {
    XrdSysMutexHelper mLock(gPhysicalIdMutex);
    gPhysicalUidCache.Purge();
    gPhysicalGidCache.Purge();
  }
  {
    XrdSysMutexHelper mLock(gPhysicalNameCacheMutex);
    gPhysicalGroupNameCache.clear();
    gPhysicalUserNameCache.clear();
    gPhysicalGroupIdCache.clear();
    gPhysicalUserIdCache.clear();
  }
  {
    XrdSysMutexHelper mLock(ActiveLock);
    ActiveTidents.clear();
  }
}


/*----------------------------------------------------------------------------*/
/**
 * Expire Active client entries which have not been used since interval
 *
 * @param interval seconds of idle time for expiration
 */

/*----------------------------------------------------------------------------*/
void
Mapping::ActiveExpire(int interval, bool force)
{
  static time_t expire = 0;
  // needs to have Active Lock locked
  time_t now = time(NULL);

  if (force || (now > expire)) {
    // expire tidents older than interval
    google::dense_hash_map<std::string, time_t>::iterator it1;
    google::dense_hash_map<std::string, time_t>::iterator it2;

    for (it1 = Mapping::ActiveTidents.begin();
         it1 != Mapping::ActiveTidents.end();) {
      if ((now - it1->second) > interval) {
        it2 = it1;
        ++it1;
        Mapping::ActiveTidents.erase(it2);
      } else {
        ++it1;
      }
    }

    Mapping::ActiveTidents.resize(0);
    expire = now + 1800;
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Map a client to its virtual identity
 *
 * @param client xrootd client authenticatino object
 * @param env opaque information containing role selection like 'eos.ruid' and 'eos.rgid'
 * @param tident trace identifier of the client
 * @param vid returned virtual identity
 */

/*----------------------------------------------------------------------------*/
void
Mapping::IdMap(const XrdSecEntity* client, const char* env, const char* tident,
               Mapping::VirtualIdentity& vid, bool log)
{
  if (!client) {
    return;
  }

  eos_static_debug("name:%s role:%s group:%s tident:%s", client->name,
                   client->role, client->grps, client->tident);
  // you first are 'nobody'
  Nobody(vid);
  XrdOucEnv Env(env);
  vid.name = client->name;
  vid.tident = tident;
  vid.sudoer = false;
  // first map by alias
  XrdOucString useralias = client->prot;
  useralias += ":";
  useralias += "\"";
  useralias += client->name;
  useralias += "\"";
  useralias += ":";
  XrdOucString groupalias = useralias;
  useralias += "uid";
  groupalias += "gid";
  RWMutexReadLock lock(gMapMutex);
  vid.prot = client->prot;

  if (vid.prot == "sss") {
    vid.key = (client->endorsements ? client->endorsements : "");
  }

  if (vid.prot == "grpc") {
    vid.key = (client->endorsements ? client->endorsements : "");
  }

  // ---------------------------------------------------------------------------
  // kerberos mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "krb5")) {
    eos_static_debug("krb5 mapping");

    if (gVirtualUidMap.count("krb5:\"<pwd>\":uid")) {
      // use physical mapping for kerberos names
      Mapping::getPhysicalIds(client->name, vid);
      vid.gid = 99;
      vid.gid_list.clear();
    }

    if (gVirtualGidMap.count("krb5:\"<pwd>\":gid")) {
      // use physical mapping for kerberos names
      uid_t uid = vid.uid;
      Mapping::getPhysicalIds(client->name, vid);
      vid.uid = uid;
      vid.uid_list.clear();
      vid.uid_list.push_back(uid);
      vid.uid_list.push_back(99);
    }
  }

  // ---------------------------------------------------------------------------
  // gsi mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "gsi")) {
    eos_static_debug("gsi mapping");

    if (gVirtualUidMap.count("gsi:\"<pwd>\":uid")) {
      // use physical mapping for gsi names
      Mapping::getPhysicalIds(client->name, vid);
      vid.gid = 99;
      vid.gid_list.clear();
    }

    if (gVirtualGidMap.count("gsi:\"<pwd>\":gid")) {
      // use physical mapping for gsi names
      uid_t uid = vid.uid;
      Mapping::getPhysicalIds(client->name, vid);
      vid.uid = uid;
      vid.uid_list.clear();
      vid.uid_list.push_back(uid);
      vid.uid_list.push_back(99);
    }

    // ---------------------------------------------------------------------------
    // VOMS mapping
    // ---------------------------------------------------------------------------
    if (client->grps) {
      std::string vomsstring = "voms:\"";
      vomsstring += client->grps;
      vomsstring += ":";
      vid.grps = client->grps;

      if (client->role) {
        // the role might be NULL
        vomsstring += client->role;
        vid.role = client->role;
      }

      vomsstring += "\"";
      std::string vomsuidstring = vomsstring;
      std::string vomsgidstring = vomsstring;
      vomsuidstring += ":uid";
      vomsgidstring += ":gid";

      // mapping to user
      if (gVirtualUidMap.count(vomsuidstring)) {
        vid.uid_list.clear();
        vid.gid_list.clear();
        // use physical mapping for VOMS roles
        // convert mapped uid to user name
        int errc = 0;
        std::string cname = Mapping::UidToUserName(gVirtualUidMap[vomsuidstring], errc);

        if (!errc) {
          Mapping::getPhysicalIds(cname.c_str(), vid);
        } else {
          Nobody(vid);
          eos_static_err("voms-mapping: cannot translate uid=%d to user name with the password db",
                         (int) gVirtualUidMap[vomsuidstring]);
        }
      }

      // mapping to group
      if (gVirtualGidMap.count(vomsgidstring)) {
        // use group mapping for VOMS roles
        vid.gid_list.clear();
        vid.gid = gVirtualGidMap[vomsgidstring];
        vid.gid_list.push_back(vid.gid);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // gsi mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "https")) {
    eos_static_debug("https mapping");

    if (gVirtualUidMap.count("https:\"<pwd>\":uid")) {
      if (gVirtualGidMap["https:\"<pwd>\":uid"] == 0) {
        // use physical mapping for https names
        Mapping::getPhysicalIds(client->name, vid);
        vid.gid = 99;
        vid.gid_list.clear();
      } else {
        vid.uid_list.clear();
        vid.uid_list.push_back(gVirtualGidMap["https:\"<pwd>\":uid"]);
        vid.uid_list.push_back(99);
        vid.gid = 99;
        vid.gid_list.clear();
      }
    }

    if (gVirtualGidMap.count("https:\"<pwd>\":gid")) {
      if (gVirtualGidMap["https:\"<pwd>\":gid"] == 0) {
        // use physical mapping for gsi names
        uid_t uid = vid.uid;
        Mapping::getPhysicalIds(client->name, vid);
        vid.uid = uid;
        vid.uid_list.clear();
        vid.uid_list.push_back(uid);
        vid.uid_list.push_back(99);
      } else {
        vid.gid_list.clear();
        vid.gid_list.push_back(gVirtualGidMap["https:\"<pwd>\":gid"]);
        vid.gid_list.push_back(99);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // sss mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "sss")) {
    eos_static_debug("sss mapping");

    if (gVirtualUidMap.count("sss:\"<pwd>\":uid")) {
      if (gVirtualUidMap["sss:\"<pwd>\":uid"] == 0) {
        eos_static_debug("sss uid mapping");
        Mapping::getPhysicalIds(client->name, vid);
        vid.gid = 99;
        vid.gid_list.clear();
      } else {
        eos_static_debug("sss uid forced mapping");
        // map to the requested id
        vid.uid_list.clear();
        vid.uid = gVirtualUidMap["sss:\"<pwd>\":uid"];
        vid.uid_list.push_back(vid.uid);

        if (vid.uid != 99) {
          vid.uid_list.push_back(99);
        }

        vid.gid_list.clear();
        vid.gid = 99;
        vid.gid_list.push_back(99);
      }
    }

    if (gVirtualGidMap.count("sss:\"<pwd>\":gid")) {
      if (gVirtualGidMap["sss:\"<pwd>\":gid"] == 0) {
        eos_static_debug("sss gid mapping");
        // use physical mapping for sss names
        uid_t uid = vid.uid;
        Mapping::getPhysicalIds(client->name, vid);
        vid.uid = uid;
        vid.uid_list.clear();
        vid.uid_list.push_back(uid);
        vid.uid_list.push_back(99);
      } else {
        eos_static_debug("sss forced gid mapping");
        // map to the requested id
        vid.gid_list.clear();
        vid.gid = gVirtualGidMap["sss:\"<pwd>\":gid"];
        vid.gid_list.push_back(vid.gid);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // unix mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "unix")) {
    eos_static_debug("unix mapping");

    if (gVirtualUidMap.count("unix:\"<pwd>\":uid")) {
      if (gVirtualUidMap["unix:\"<pwd>\":uid"] == 0) {
        eos_static_debug("unix uid mapping");
        // use physical mapping for unix names
        Mapping::getPhysicalIds(client->name, vid);
        vid.gid = 99;
        vid.gid_list.clear();
      } else {
        eos_static_debug("unix uid forced mapping");
        // map to the requested id
        vid.uid_list.clear();
        vid.uid = gVirtualUidMap["unix:\"<pwd>\":uid"];
        vid.uid_list.push_back(vid.uid);

        if (vid.uid != 99) {
          vid.uid_list.push_back(99);
        }

        vid.gid_list.clear();
        vid.gid = 99;
        vid.gid_list.push_back(99);
      }
    }

    if (gVirtualGidMap.count("unix:\"<pwd>\":gid")) {
      if (gVirtualGidMap["unix:\"<pwd>\":gid"] == 0) {
        eos_static_debug("unix gid mapping");
        // use physical mapping for unix names
        uid_t uid = vid.uid;
        Mapping::getPhysicalIds(client->name, vid);
        vid.uid = uid;
        vid.uid_list.clear();
        vid.uid_list.push_back(uid);
        vid.uid_list.push_back(99);
      } else {
        eos_static_debug("unix forced gid mapping");
        // map to the requested id
        vid.gid_list.clear();
        vid.gid = gVirtualGidMap["unix:\"<pwd>\":gid"];
        vid.gid_list.push_back(vid.gid);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // tident mapping
  // ---------------------------------------------------------------------------
  XrdOucString mytident = "";
  XrdOucString myrole = "";
  XrdOucString wildcardtident = "";
  XrdOucString host = "";
  XrdOucString stident = "tident:";
  stident += "\"";
  stident += ReduceTident(vid.tident, wildcardtident, mytident, host);

  if (host == "127.0.0.1") {
    host = "localhost";
  }

  myrole = mytident;
  myrole.erase(mytident.find("@"));
  // FUSE select's now the role via <uid>[:connectionid]
  // the connection id is already removed by ReduceTident
  myrole.erase(myrole.find("."));
  XrdOucString swctident = "tident:";
  swctident += "\"";
  swctident += wildcardtident;
  XrdOucString suidtident = stident;
  suidtident += "\":uid";
  XrdOucString sgidtident = stident;
  sgidtident += "\":gid";
  XrdOucString swcuidtident = swctident;
  swcuidtident += "\":uid";
  XrdOucString swcgidtident = swctident;
  swcgidtident += "\":gid";
  XrdOucString sprotuidtident = swcuidtident;
  XrdOucString sprotgidtident = swcgidtident;
  sprotuidtident.replace("*",
                         vid.prot); // there can be a protocol specific rule like sss:@<host>:uid...
  sprotgidtident.replace("*",
                         vid.prot); // there can be a protocol specific rule like sss:@<host>:gid...
  eos_static_debug("swcuidtident=%s sprotuidtident=%s myrole=%s",
                   swcuidtident.c_str(), sprotuidtident.c_str(), myrole.c_str());

  if ((gVirtualUidMap.count(suidtident.c_str()))) {
    //    eos_static_debug("tident mapping");
    vid.uid = gVirtualUidMap[suidtident.c_str()];

    if (!HasUid(vid.uid, vid.uid_list)) {
      vid.uid_list.push_back(vid.uid);
    }

    if (!HasUid(99, vid.uid_list)) {
      vid.uid_list.push_back(99);
    }
  }

  if ((gVirtualGidMap.count(sgidtident.c_str()))) {
    //    eos_static_debug("tident mapping");
    vid.gid = gVirtualGidMap[sgidtident.c_str()];

    if (!HasGid(vid.gid, vid.gid_list)) {
      vid.gid_list.push_back(vid.gid);
    }

    if (!HasGid(99, vid.gid_list)) {
      vid.gid_list.push_back(99);
    }
  }

  // ---------------------------------------------------------------------------
  // wild card tidents/protocol tidents
  // one can define mapping entries like '*@host:uid=>0' e.g. for fuse mounts
  // or only for a certain protocol like 'sss@host:uid=>0'
  // ---------------------------------------------------------------------------
  XrdOucString tuid = "";
  XrdOucString tgid = "";

  if (gVirtualUidMap.count(swcuidtident.c_str())) {
    // there is an entry like "*@<host:uid" matching all protocols
    tuid = swcuidtident.c_str();
  } else {
    if (gVirtualUidMap.count(sprotuidtident.c_str())) {
      // there is a protocol specific entry "<prot>@<host>:uid"
      tuid = sprotuidtident.c_str();
    } else {
      if (gAllowedTidentMatches.size()) {
        std::string sprot = vid.prot.c_str();

        for (auto it = gAllowedTidentMatches.begin(); it != gAllowedTidentMatches.end();
             ++it) {
          if (sprot != it->first.c_str()) {
            continue;
          }

          if (host.matches(it->second.c_str())) {
            sprotuidtident.replace(host.c_str(), it->second.c_str());

            if (gVirtualUidMap.count(sprotuidtident.c_str())) {
              tuid = sprotuidtident.c_str();
              break;
            }
          }
        }
      }
    }
  }

  if (gVirtualGidMap.count(swcgidtident.c_str())) {
    // there is an entry like "*@<host>:gid" matching all protocols
    tgid = swcgidtident.c_str();
  } else {
    if (gVirtualGidMap.count(sprotgidtident.c_str())) {
      // there is a protocol specific entry "<prot>@<host>:uid"
      tgid = sprotgidtident.c_str();
    } else {
      if (gAllowedTidentMatches.size()) {
        std::string sprot = vid.prot.c_str();

        for (auto it = gAllowedTidentMatches.begin(); it != gAllowedTidentMatches.end();
             ++it) {
          if (sprot != it->first.c_str()) {
            continue;
          }

          if (host.matches(it->second.c_str())) {
            sprotuidtident.replace(host.c_str(), it->second.c_str());

            if (gVirtualUidMap.count(sprotuidtident.c_str())) {
              tuid = sprotuidtident.c_str();
              break;
            }
          }
        }
      }
    }
  }

  eos_static_debug("tuid=%s tgid=%s", tuid.c_str(), tgid.c_str());

  if (gVirtualUidMap.count(tuid.c_str())) {
    if (!gVirtualUidMap[tuid.c_str()]) {
      if (gRootSquash && (host != "localhost") && (host != "localhost.localdomain") &&
          (host != "localhost6.localdomain6") && (vid.name == "root") &&
          (myrole == "root")) {
        eos_static_debug("tident root uid squash");
        vid.uid_list.clear();
        vid.uid_list.push_back(DAEMONUID);
        vid.uid = DAEMONUID;
        vid.gid_list.clear();
        vid.gid = DAEMONGID;
        vid.gid_list.push_back(DAEMONGID);
      } else {
        eos_static_debug("tident uid mapping prot=%s name=%s",
                         vid.prot.c_str(), vid.name.c_str());
        vid.uid_list.clear();

        // use physical mapping
        // unix protocol maps to the role if the client is the root account
        // otherwise it maps to the unix ID on the client host
        if (((vid.prot == "unix") && (vid.name == "root")) ||
            ((vid.prot == "sss") && (vid.name == "daemon"))) {
          Mapping::getPhysicalIds(myrole.c_str(), vid);
        } else {
          Mapping::getPhysicalIds(client->name, vid);
        }
      }
    } else {
      eos_static_debug("tident uid forced mapping");
      // map to the requested id
      vid.uid_list.clear();
      vid.uid = gVirtualUidMap[tuid.c_str()];
      vid.uid_list.push_back(vid.uid);

      if (vid.uid != 99) {
        vid.uid_list.push_back(99);
      }

      vid.gid_list.clear();
      vid.gid = 99;
      vid.gid_list.push_back(99);
    }
  }

  if (gVirtualGidMap.count(tgid.c_str())) {
    if (!gVirtualGidMap[tgid.c_str()]) {
      if (gRootSquash && (host != "localhost") && (host != "localhost.localdomain") &&
          (vid.name == "root") && (myrole == "root")) {
        eos_static_debug("tident root gid squash");
        vid.gid_list.clear();
        vid.gid_list.push_back(DAEMONGID);
        vid.gid = DAEMONGID;
      } else {
        eos_static_debug("tident gid mapping");
        uid_t uid = vid.uid;

        if (((vid.prot == "unix") && (vid.name == "root")) ||
            ((vid.prot == "sss") && (vid.name == "daemon"))) {
          Mapping::getPhysicalIds(myrole.c_str(), vid);
        } else {
          Mapping::getPhysicalIds(client->name, vid);
        }

        vid.uid = uid;
        vid.uid_list.clear();
        vid.uid_list.push_back(uid);
        vid.uid_list.push_back(99);
      }
    } else {
      eos_static_debug("tident gid forced mapping");
      // map to the requested id
      vid.gid_list.clear();
      vid.gid = gVirtualGidMap[tgid.c_str()];
      vid.gid_list.push_back(vid.gid);
    }
  }

  eos_static_debug("suidtident:%s sgidtident:%s", suidtident.c_str(),
                   sgidtident.c_str());

  // ---------------------------------------------------------------------------
  // the configuration door for localhost clients adds always the adm/adm vid's
  // ---------------------------------------------------------------------------
  if ((suidtident == "tident:\"root@localhost.localdomain\":uid") ||
      (suidtident == "tident:\"root@localhost\":uid")) {
    vid.sudoer = true;
    vid.uid = 3;
    vid.gid = 4;

    if (!HasUid(3, vid.uid_list)) {
      vid.uid_list.push_back(vid.uid);
    }

    if (!HasGid(4, vid.gid_list)) {
      vid.gid_list.push_back(vid.gid);
    }
  }

  // ---------------------------------------------------------------------------
  // grpc key mapping
  // ---------------------------------------------------------------------------
  if ((vid.prot == "grpc") && vid.key.length()) {
    std::string keyname = vid.key.c_str();
    std::string maptident = "tident:\"grpc@";
    std::string wildcardmaptident = "tident:\"grpc@*\":uid";
    std::vector<std::string> vtident;
    eos::common::StringConversion::Tokenize(client->tident, vtident, "@");

    if (vtident.size() == 2) {
      maptident += vtident[1];
    }

    maptident += "\":uid";
    eos_static_info("%d %s %s %s", vtident.size(), client->tident,
                    maptident.c_str(), wildcardmaptident.c_str());

    if (gVirtualUidMap.count(maptident.c_str()) ||
        gVirtualUidMap.count(wildcardmaptident.c_str())) {
      // if this is an allowed gateway, map according to client name or authkey
      std::string uidkey = "grpc:\"";
      uidkey += keyname;
      uidkey += "\":uid";
      vid.uid = 99;
      vid.uid_list.clear();
      vid.uid_list.push_back(99);

      if (gVirtualUidMap.count(uidkey.c_str())) {
        vid.uid = gVirtualUidMap[uidkey.c_str()];
        vid.uid_list.push_back(vid.uid);
      }

      std::string gidkey = "grpc:\"";
      gidkey += keyname;
      gidkey += "\":gid";
      vid.gid = 99;
      vid.gid_list.clear();
      vid.gid_list.push_back(99);

      if (gVirtualGidMap.count(gidkey.c_str())) {
        vid.gid = gVirtualGidMap[gidkey.c_str()];
        vid.gid_list.push_back(vid.gid);
      }
    } else {
      // we are nobody if we are not an authorized host
      Nobody(vid);
    }
  }

  // ---------------------------------------------------------------------------
  // explicit virtual mapping overrules physical mappings - the second one comes from the physical mapping before
  // ---------------------------------------------------------------------------
  vid.uid = (gVirtualUidMap.count(useralias.c_str())) ?
            gVirtualUidMap[useralias.c_str() ] : vid.uid;

  if (!HasUid(vid.uid, vid.uid_list)) {
    vid.uid_list.insert(vid.uid_list.begin(), vid.uid);
  }

  vid.gid = (gVirtualGidMap.count(groupalias.c_str())) ?
            gVirtualGidMap[groupalias.c_str()] : vid.gid;

  // eos_static_debug("mapped %d %d", vid.uid,vid.gid);

  if (!HasGid(vid.gid, vid.gid_list)) {
    vid.gid_list.insert(vid.gid_list.begin(), vid.gid);
  }

  // ---------------------------------------------------------------------------
  // add virtual user and group roles - if any
  // ---------------------------------------------------------------------------
  if (gUserRoleVector.count(vid.uid)) {
    uid_vector::const_iterator it;

    for (it = gUserRoleVector[vid.uid].begin();
         it != gUserRoleVector[vid.uid].end(); ++it)
      if (!HasUid((*it), vid.uid_list)) {
        vid.uid_list.push_back((*it));
      }
  }

  if (gGroupRoleVector.count(vid.uid)) {
    gid_vector::const_iterator it;

    for (it = gGroupRoleVector[vid.uid].begin();
         it != gGroupRoleVector[vid.uid].end(); ++it)
      if (!HasGid((*it), vid.gid_list)) {
        vid.gid_list.push_back((*it));
      }
  }

  // ---------------------------------------------------------------------------
  // Environment selected roles
  // ---------------------------------------------------------------------------
  XrdOucString ruid = Env.Get("eos.ruid");
  XrdOucString rgid = Env.Get("eos.rgid");
  XrdOucString rapp = Env.Get("eos.app");
  uid_t sel_uid = vid.uid;
  uid_t sel_gid = vid.gid;

  if (ruid.length()) {
    if (!IsUid(ruid, sel_uid)) {
      int errc = 0;
      // try alias conversion
      std::string luid = ruid.c_str();
      sel_uid = (gVirtualUidMap.count(ruid.c_str())) ? gVirtualUidMap[ruid.c_str() ] :
                99;

      if (sel_uid == 99) {
        sel_uid = UserNameToUid(luid, errc);
      }

      if (errc) {
        sel_uid = 99;
      }
    }
  }

  if (rgid.length()) {
    if (!IsGid(rgid, sel_gid)) {
      int errc = 0;
      // try alias conversion
      std::string lgid = rgid.c_str();
      sel_gid = (gVirtualGidMap.count(rgid.c_str())) ? gVirtualGidMap[rgid.c_str()] :
                99;

      if (sel_gid == 99) {
        sel_gid = GroupNameToGid(lgid, errc);
      }

      if (errc) {
        sel_gid = 99;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Sudoer flag setting
  // ---------------------------------------------------------------------------
  if (gSudoerMap.count(vid.uid)) {
    vid.sudoer = true;
  }

  // ---------------------------------------------------------------------------
  // Check if we are allowed to take sel_uid & sel_gid
  // ---------------------------------------------------------------------------
  if (!vid.sudoer) {
    // if we are not a sudore, scan the allowed ids
    if (HasUid(sel_uid, vid.uid_list)) {
      vid.uid = sel_uid;
    } else {
      vid.uid = 99;
    }

    if (HasGid(sel_gid, vid.gid_list)) {
      vid.gid = sel_gid;
    } else {
      vid.gid = 99;
    }
  } else {
    vid.uid = sel_uid;
    vid.gid = sel_gid;

    if (ruid.length() || rgid.length()) {
      if (!eos::common::Mapping::HasGid(sel_gid, vid)) {
        vid.gid_list.push_back(sel_gid);
      }

      if (!eos::common::Mapping::HasUid(sel_uid, vid)) {
        vid.uid_list.push_back(sel_uid);
      }
    }
  }

  if (client->host) {
    vid.host = client->host;
  } else {
    vid.host = host.c_str();
  }

  size_t dotpos = vid.host.find(".");

  // remove hostname
  if (dotpos != std::string::npos) {
    vid.domain = vid.host.substr(dotpos + 1);
  } else {
    vid.domain = "localdomain";
  }

  {
    int errc = 0;
    // add the uid/gid as strings
    vid.uid_string = UidToUserName(vid.uid, errc);
    vid.gid_string = GidToGroupName(vid.gid, errc);
  }

  if (rapp.length()) {
    vid.app = rapp.c_str();
  }

  time_t now = time(NULL);

  // ---------------------------------------------------------------------------
  // Check the Geo Location
  // ---------------------------------------------------------------------------
  if ((!vid.geolocation.length()) && (gGeoMap.size())) {
    // if the geo location was not set externally and we have some recipe we try
    // to translate the host name and match a rule

    // if we have a default geo location we assume that a client in that one
    if (gGeoMap.count("default")) {
      vid.geolocation = gGeoMap["default"];
    }

    std::string ipstring = gIpCache.GetIp(host.c_str());

    if (ipstring.length()) {
      std::string sipstring = ipstring;
      GeoLocationMap_t::const_iterator it;
      GeoLocationMap_t::const_iterator longuestmatch = gGeoMap.end();

      // we use the geo location with the longest name match
      for (it = gGeoMap.begin(); it != gGeoMap.end(); ++it) {
        // if we have a previously matched geoloc and if it's longer that the current one, try the next one
        if (longuestmatch != gGeoMap.end() &&
            it->first.length() <= longuestmatch->first.length()) {
          continue;
        }

        if (sipstring.compare(0, it->first.length(), it->first) == 0) {
          vid.geolocation = it->second;
          longuestmatch = it;
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Maintain the active client map and expire old entries
  // ---------------------------------------------------------------------------
  ActiveLock.Lock();

  // ---------------------------------------------------------------------------
  // safty measures not to exceed memory by 'nasty' clients
  // ---------------------------------------------------------------------------
  if (ActiveTidents.size() > 25000) {
    ActiveExpire();
  }

  if (ActiveTidents.size() < 60000) {
    char actident[1024];
    snprintf(actident, sizeof(actident) - 1, "%d^%s^%s^%s^%s", vid.uid,
             mytident.c_str(), vid.prot.c_str(), vid.host.c_str(), vid.app.c_str());
    std::string intident = actident;
    ActiveTidents[intident] = now;
  }

  ActiveLock.UnLock();
  eos_static_debug("selected %d %d [%s %s]", vid.uid, vid.gid, ruid.c_str(),
                   rgid.c_str());

  if (log) {
    eos_static_info("%s sec.tident=\"%s\"", eos::common::SecEntity::ToString(client,
                    Env.Get("eos.app")).c_str(), tident);
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Print the current mappings
 *
 * @param stdOut the output is stored here
 * @param option can be 'u' for user role mappings 'g' for group role mappings 's' for sudoer list 'U' for user alias mapping 'G' for group alias mapping 'y' for gateway mappings (tidents) 'a' for authentication mapping rules 'l' for geo location rules
 */

/*----------------------------------------------------------------------------*/
void
Mapping::Print(XrdOucString& stdOut, XrdOucString option)
{
  bool translateids = true;

  if (option.find("n") != STR_NPOS) {
    translateids = false;
    option.replace("n", "");
  }

  if ((!option.length()) || ((option.find("u")) != STR_NPOS)) {
    UserRoleMap_t::const_iterator it;

    for (it = gUserRoleVector.begin(); it != gUserRoleVector.end(); ++it) {
      char iuid[4096];
      sprintf(iuid, "%d", it->first);
      char suid[4096];
      sprintf(suid, "%-6s", iuid);

      if (translateids) {
        int errc = 0;
        std::string username = UidToUserName(it->first, errc);

        if (!errc) {
          sprintf(suid, "%-12s", username.c_str());
        }
      }

      stdOut += "membership uid: ";
      stdOut += suid;
      stdOut += " => uids(";

      for (unsigned int i = 0; i < (it->second).size(); i++) {
        if (translateids) {
          int errc = 0;
          std::string username = UidToUserName(it->second[i], errc);

          if (!errc) {
            stdOut += username.c_str();
          } else {
            stdOut += (int)(it->second)[i];
          }
        } else {
          stdOut += (int)(it->second)[i];
        }

        if (i < ((it->second).size() - 1)) {
          stdOut += ",";
        }
      }

      stdOut += ")\n";
    }
  }

  if ((!option.length()) || ((option.find("g")) != STR_NPOS)) {
    UserRoleMap_t::const_iterator it;

    for (it = gGroupRoleVector.begin(); it != gGroupRoleVector.end(); ++it) {
      char iuid[4096];
      sprintf(iuid, "%d", it->first);
      char suid[4096];
      sprintf(suid, "%-6s", iuid);

      if (translateids) {
        int errc = 0;
        std::string username = UidToUserName(it->first, errc);

        if (!errc) {
          sprintf(suid, "%-12s", username.c_str());
        }
      }

      stdOut += "membership uid: ";
      stdOut += suid;
      stdOut += " => gids(";

      for (unsigned int i = 0; i < (it->second).size(); i++) {
        if (translateids) {
          int errc = 0;
          std::string username = GidToGroupName(it->second[i], errc);

          if (!errc) {
            stdOut += username.c_str();
          } else {
            stdOut += (int)(it->second)[i];
          }
        } else {
          stdOut += (int)(it->second)[i];
        }

        if (i < ((it->second).size() - 1)) {
          stdOut += ",";
        }
      }

      stdOut += ")\n";
    }
  }

  if ((!option.length()) || ((option.find("s")) != STR_NPOS)) {
    SudoerMap_t::const_iterator it;
    // print sudoer line
    stdOut += "sudoer                 => uids(";

    for (it = gSudoerMap.begin(); it != gSudoerMap.end(); ++it) {
      if (it->second) {
        int errc = 0;
        std::string username = UidToUserName(it->first, errc);

        if (!errc && translateids) {
          stdOut += username.c_str();
        } else {
          stdOut += (int)(it->first);
        }

        stdOut += ",";
      }
    }

    if (stdOut.endswith(",")) {
      stdOut.erase(stdOut.length() - 1);
    }

    stdOut += ")\n";
  }

  if ((!option.length()) || ((option.find("U")) != STR_NPOS)) {
    VirtualUserMap_t::const_iterator it;

    for (it = gVirtualUidMap.begin(); it != gVirtualUidMap.end(); ++it) {
      stdOut += it->first.c_str();
      stdOut += " => ";
      int errc = 0;
      std::string username = UidToUserName(it->second, errc);

      if (!errc && translateids) {
        stdOut += username.c_str();
      } else {
        stdOut += (int) it->second;
      }

      stdOut += "\n";
    }
  }

  if ((!option.length()) || ((option.find("G")) != STR_NPOS)) {
    VirtualGroupMap_t::const_iterator it;

    for (it = gVirtualGidMap.begin(); it != gVirtualGidMap.end(); ++it) {
      stdOut += it->first.c_str();
      stdOut += " => ";
      int errc = 0;
      std::string groupname = GidToGroupName(it->second, errc);

      if (!errc && translateids) {
        stdOut += groupname.c_str();
      } else {
        stdOut += (int) it->second;
      }

      stdOut += "\n";
    }
  }

  if (((option.find("y")) != STR_NPOS)) {
    VirtualUserMap_t::const_iterator it;

    for (it = gVirtualUidMap.begin(); it != gVirtualUidMap.end(); ++it) {
      if (!it->second) {
        XrdOucString authmethod = it->first.c_str();

        if (!authmethod.beginswith("tident:")) {
          continue;
        }

        int dpos = authmethod.find("@");
        authmethod.erase(0, dpos + 1);
        dpos = authmethod.find("\"");
        authmethod.erase(dpos);
        stdOut += "gateway=";
        stdOut += authmethod;
        stdOut += "\n";
      }
    }
  }

  if (((option.find("a")) != STR_NPOS)) {
    VirtualUserMap_t::const_iterator it;

    for (it = gVirtualUidMap.begin(); it != gVirtualUidMap.end(); ++it) {
      if (!it->second) {
        XrdOucString authmethod = it->first.c_str();

        if (authmethod.beginswith("tident:")) {
          continue;
        }

        int dpos = authmethod.find(":");
        authmethod.erase(dpos);
        stdOut += "auth=";
        stdOut += authmethod;
        stdOut += "\n";
      }
    }
  }

  if ((!option.length()) || ((option.find("l")) != STR_NPOS)) {
    for (auto it = gGeoMap.begin(); it != gGeoMap.end(); ++it) {
      char sline[1024];
      snprintf(sline, sizeof(sline) - 1, "geotag:\"%s\" => \"%s\"\n",
               it->first.c_str(), it->second.c_str());
      stdOut += sline;
    }
  }

  if ((!option.length())) {
    for (auto it = gAllowedTidentMatches.begin(); it != gAllowedTidentMatches.end();
         ++it) {
      char sline[1024];
      snprintf(sline, sizeof(sline) - 1, "hostmatch:\"protocol=%s pattern=%s\n",
               it->first.c_str(), it->second.c_str());
      stdOut += sline;
    }
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Store the physical Ids for name in the virtual identity
 *
 * @param name user name
 * @param vid virtual identity to store
 */

/*----------------------------------------------------------------------------*/
void
Mapping::getPhysicalIds(const char* name, VirtualIdentity& vid)
{
  struct passwd passwdinfo;
  char buffer[131072];

  if (!name) {
    return;
  }

  gid_vector* gv;
  id_pair* id = 0;
  memset(&passwdinfo, 0, sizeof(passwdinfo));
  eos_static_debug("find in uid cache %s", name);
  XrdSysMutexHelper gLock(gPhysicalIdMutex);

  // cache short cut's
  if (!(id = gPhysicalUidCache.Find(name))) {
    eos_static_debug("not found in uid cache");
    XrdOucString sname = name;
    bool use_pw = true;

    if (sname.length() == 8) {
      bool known_tident = false;

      if (sname.beginswith("*") || sname.beginswith("~")) {
        known_tident = true;
        // that is a new base-64 encoded id following the format '*1234567'
        // where 1234567 is the base64 encoded 42-bit value of 20-bit uid |
        // 16-bit gid | 6-bit session id.
        XrdOucString b64name = sname;
        b64name.erase(0, 1);
        // Decoden '_' -> '/', '-' -> '+' that was done to ensure the validity
        // of the XRootD URL.
        b64name.replace('_', '/');
        b64name.replace('-', '+');
        b64name += "=";
        unsigned long long bituser = 0;
        char* out = 0;
        size_t outlen;

        if (eos::common::SymKey::Base64Decode(b64name, out, outlen)) {
          if (outlen <= 8) {
            memcpy((((char*) &bituser)) + 8 - outlen, out, outlen);
            eos_static_debug("msg=\"decoded base-64 uid/gid/sid\" val=%llx val=%llx",
                             bituser, n_tohll(bituser));
          } else {
            eos_static_err("msg=\"decoded base-64 uid/gid/sid too long\" len=%d", outlen);
            delete id;
            return;
          }

          bituser = n_tohll(bituser);

          if (out) {
            free(out);
          }

          if (id) {
            delete id;
          }

          if (sname.beginswith("*")) {
            id = new id_pair((bituser >> 22) & 0xfffff, (bituser >> 6) & 0xffff);
          } else {
            // only user id got forwarded, we retrieve the corresponding group
            uid_t ruid = (bituser >> 6) & 0xfffffffff;
            gPhysicalIdMutex.UnLock();
            struct passwd* pwbufp = 0;

            if (getpwuid_r(ruid, &passwdinfo, buffer, 16384, &pwbufp) || (!pwbufp)) {
              gPhysicalIdMutex.Lock();
              return;
            }

            id = new id_pair(passwdinfo.pw_uid, passwdinfo.pw_gid);
          }

          eos_static_debug("using base64 mapping %s %d %d", sname.c_str(), id->uid,
                           id->gid);
        } else {
          eos_static_err("msg=\"failed to decoded base-64 uid/gid/sid\" id=%s",
                         sname.c_str());
          gPhysicalIdMutex.UnLock();
          delete id;
          return;
        }
      }

      if (known_tident) {
        if (gRootSquash && (!id->uid || !id->gid)) {
          return;
        }

        vid.uid = id->uid;
        vid.gid = id->gid;
        vid.uid_list.clear();
        vid.uid_list.push_back(vid.uid);
        vid.gid_list.clear();
        vid.gid_list.push_back(vid.gid);
        gid_vector* vec = new uid_vector;
        *vec = vid.gid_list;
        gPhysicalUidCache.Add(name, id, 3600);
        eos_static_debug("adding to cache uid=%u gid=%u", id->uid, id->gid);
        gPhysicalGidCache.Add(name, vec, 3600);
        use_pw = false;
      }
    }

    if (use_pw) {
      gPhysicalIdMutex.UnLock();
      struct passwd* pwbufp = 0;
      {
        if (getpwnam_r(name, &passwdinfo, buffer, 16384, &pwbufp) || (!pwbufp)) {
          gPhysicalIdMutex.Lock();
          return;
        }
      }
      gPhysicalIdMutex.Lock();
      id = new id_pair(passwdinfo.pw_uid, passwdinfo.pw_gid);
      gPhysicalUidCache.Add(name, id, 3600);
      eos_static_debug("adding to cache uid=%u gid=%u", id->uid, id->gid);
    }

    if (!id) {
      return;
    }
  }

  vid.uid = id->uid;
  vid.gid = id->gid;

  if ((gv = gPhysicalGidCache.Find(name))) {
    vid.uid_list.push_back(id->uid);
    vid.gid_list = *gv;
    vid.uid = id->uid;
    vid.gid = id->gid;
    eos_static_debug("returning uid=%u gid=%u", id->uid, id->gid);
    return;
  }

  std::string secondary_groups = getenv("EOS_SECONDARY_GROUPS") ?
                                 getenv("EOS_SECONDARY_GROUPS") : "";

  if (secondary_groups.length() && (secondary_groups == "1")) {
    struct group* gr;
    eos_static_debug("group lookup");
    gid_t gid = id->gid;
    setgrent();

    while ((gr = getgrent())) {
      int cnt;
      cnt = 0;

      if (gr->gr_gid == gid) {
        if (!vid.gid_list.size()) {
          vid.gid_list.push_back(gid);
          vid.gid = gid;
        }
      }

      while (gr->gr_mem[cnt]) {
        if (!strcmp(gr->gr_mem[cnt], name)) {
          vid.gid_list.push_back(gr->gr_gid);
        }

        cnt++;
      }
    }

    endgrent();
  }

  // add to the cache
  gid_vector* vec = new uid_vector;
  *vec = vid.gid_list;
  gPhysicalGidCache.Add(name, vec, 3600);
  return;
}

/*----------------------------------------------------------------------------*/
/**
 * Convert uid to user name
 *
 * @param uid unix user id
 * @param errc 0 if success, EINVAL if does not exist
 *
 * @return user name as string
 */

/*----------------------------------------------------------------------------*/
std::string
Mapping::UidToUserName(uid_t uid, int& errc)
{
  errc = 0;
  {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);

    if (gPhysicalUserNameCache.count(uid)) {
      return gPhysicalUserNameCache[uid];
    }
  }
  char buffer[131072];
  int buflen = sizeof(buffer);
  std::string uid_string = "";
  struct passwd pwbuf;
  struct passwd* pwbufp = 0;
  (void) getpwuid_r(uid, &pwbuf, buffer, buflen, &pwbufp);

  if (pwbufp == NULL) {
    char buffer[131072];
    int buflen = sizeof(buffer);
    std::string uid_string = "";
    struct passwd pwbuf;
    struct passwd* pwbufp = 0;
    {
      if (getpwuid_r(uid, &pwbuf, buffer, buflen, &pwbufp) || (!pwbufp)) {
        char suid[1024];
        snprintf(suid, sizeof(suid) - 1, "%u", uid);
        uid_string = suid;
        errc = EINVAL;
        return uid_string; // don't cache this one
      } else {
        uid_string = pwbuf.pw_name;
        errc = 0;
      }
    }
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);
    gPhysicalUserNameCache[uid] = uid_string;
    gPhysicalUserIdCache[uid_string] = uid;
    return uid_string;
  } else {
    uid_string = pwbuf.pw_name;
    errc = 0;
  }

  XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);
  gPhysicalUserNameCache[uid] = uid_string;
  gPhysicalUserIdCache[uid_string] = uid;
  return uid_string;
}

/*----------------------------------------------------------------------------*/
/**
 * Convert gid to group name
 *
 * @param gid unix group id
 * @param errc 0 if success, EINVAL if does not exist
 *
 * @return user name as string
 */

/*----------------------------------------------------------------------------*/
std::string
Mapping::GidToGroupName(gid_t gid, int& errc)
{
  errc = 0;
  {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);

    if (gPhysicalGroupNameCache.count(gid)) {
      return gPhysicalGroupNameCache[gid];
    }
  }
  {
    char buffer[131072];
    int buflen = sizeof(buffer);
    struct group grbuf;
    struct group* grbufp = 0;
    std::string gid_string = "";

    if (getgrgid_r(gid, &grbuf, buffer, buflen, &grbufp) || (!grbufp)) {
      // cannot translate this name
      char sgid[1024];
      snprintf(sgid, sizeof(sgid) - 1, "%u", gid);
      gid_string = sgid;
      errc = EINVAL;
      return gid_string; // don't cache this one
    } else {
      gid_string = grbuf.gr_name;
      errc = 0;
    }

    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);
    gPhysicalGroupNameCache[gid] = gid_string;
    gPhysicalGroupIdCache[gid_string] = gid;
    return gid_string;
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Convert string name to uid
 *
 * @param username name as string
 * @param errc 0 if success, EINVAL if does not exist
 *
 * @return user id
 */

/*----------------------------------------------------------------------------*/
uid_t
Mapping::UserNameToUid(const std::string& username, int& errc)
{
  {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);

    if (gPhysicalUserIdCache.count(username)) {
      return gPhysicalUserIdCache[username];
    }
  }
  char buffer[131072];
  int buflen = sizeof(buffer);
  uid_t uid = 99;
  struct passwd pwbuf;
  struct passwd* pwbufp = 0;
  errc = 0;
  (void) getpwnam_r(username.c_str(), &pwbuf, buffer, buflen, &pwbufp);

  if (pwbufp == NULL) {
    bool is_number = true;

    for (size_t i = 0; i < username.length(); i++) {
      if (!isdigit(username[i])) {
        is_number = false;
        break;
      }
    }

    uid = atoi(username.c_str());

    if ((uid != 0) && (is_number)) {
      errc = 0;
      return uid;
    } else {
      errc = EINVAL;
      uid = 99;
      return uid;
    }
  } else {
    uid = pwbuf.pw_uid;
    errc = 0;
  }

  if (!errc) {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);
    gPhysicalUserIdCache[username] = uid;
    gPhysicalUserNameCache[uid] = username;
  }

  return uid;
}

/*----------------------------------------------------------------------------*/
/**
 * Convert string name to gid
 *
 * @param groupname name as string
 * @param errc 0 if success, EINVAL if does not exist
 *
 * @return group id
 */

/*----------------------------------------------------------------------------*/
gid_t
Mapping::GroupNameToGid(const std::string& groupname, int& errc)
{
  {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);

    if (gPhysicalGroupIdCache.count(groupname)) {
      return gPhysicalGroupIdCache[groupname];
    }
  }
  char buffer[131072];
  int buflen = sizeof(buffer);
  struct group grbuf;
  struct group* grbufp = 0;
  gid_t gid = 99;
  errc = 0;
  (void) getgrnam_r(groupname.c_str(), &grbuf, buffer, buflen, &grbufp);

  if (!grbufp) {
    bool is_number = true;

    for (size_t i = 0; i < groupname.length(); i++) {
      if (!isdigit(groupname[i])) {
        is_number = false;
        break;
      }
    }

    gid = atoi(groupname.c_str());

    if ((gid != 0) && (is_number)) {
      errc = 0;
      return gid;
    } else {
      errc = EINVAL;
      gid = 99;
    }
  } else {
    gid = grbuf.gr_gid;
    errc = 0;
  }

  if (!errc) {
    XrdSysMutexHelper cMutex(gPhysicalNameCacheMutex);
    gPhysicalGroupIdCache[groupname] = gid;
    gPhysicalGroupNameCache[gid] = groupname;
  }

  return gid;
}

/*----------------------------------------------------------------------------*/
/**
 * Convert string name to gid
 *
 * @param groupname name as string
 * @param errc 0 if success, EINVAL if does not exist
 *
 * @return group id
 */

/*----------------------------------------------------------------------------*/

std::string
Mapping::ip_cache::GetIp(const char* hostname)
{
  time_t now = time(NULL);
  {
    // check for an existing translation
    RWMutexReadLock guard(mLocker);

    if (mIp2HostMap.count(hostname) &&
        mIp2HostMap[hostname].first > now) {
      eos_static_debug("status=cached host=%s ip=%s", hostname,
                       mIp2HostMap[hostname].second.c_str());
      // give cached entry
      return mIp2HostMap[hostname].second;
    }
  }
  {
    // refresh an entry
    unsigned int ipaddr;

    if (XrdSysDNS::Host2IP(hostname, &ipaddr) == 1) {
      char ipstring[64];
      int hostlen = XrdSysDNS::IP2String(ipaddr, 0, ipstring, 64);

      if (hostlen > 0) {
        RWMutexWriteLock guard(mLocker);
        std::string sip = ipstring;
        mIp2HostMap[hostname] = std::make_pair(now + mLifeTime, sip);
        eos_static_debug("status=refresh host=%s ip=%s", hostname,
                         mIp2HostMap[hostname].second.c_str());
        return sip;
      }
    }

    return "";
  }
}

// -----------------------------------------------------------------------------
//! Convert a komma separated uid string to a vector uid list
// -----------------------------------------------------------------------------

void
Mapping::KommaListToUidVector(const char* list, std::vector<uid_t>& vector_list)
{
  XrdOucString slist = list;
  XrdOucString number = "";
  int kommapos;

  if (!slist.endswith(",")) {
    slist += ",";
  }

  do {
    kommapos = slist.find(",");

    if (kommapos != STR_NPOS) {
      number.assign(slist, 0, kommapos - 1);
      int errc;
      std::string username = number.c_str();
      uid_t uid = 99;

      if (std::find_if(username.begin(), username.end(),
      [](unsigned char c) {
      return std::isalpha(c);
      }) !=
      username.end()) {
        uid = eos::common::Mapping::UserNameToUid(username, errc);
      } else {
        try {
          uid = std::stoul(username);
        } catch (const std::exception& e) {
          uid = 99;
        }
      }

      if (!errc) {
        vector_list.push_back(uid);
      }

      slist.erase(0, kommapos + 1);
    }
  } while (kommapos != STR_NPOS);
}

// -----------------------------------------------------------------------------
//! Convert a komma separated gid string to a vector gid list
// -----------------------------------------------------------------------------

void
Mapping::KommaListToGidVector(const char* list, std::vector<gid_t>& vector_list)
{
  XrdOucString slist = list;
  XrdOucString number = "";
  int kommapos;

  if (!slist.endswith(",")) {
    slist += ",";
  }

  do {
    kommapos = slist.find(",");

    if (kommapos != STR_NPOS) {
      number.assign(slist, 0, kommapos - 1);
      int errc;
      std::string groupname = number.c_str();
      gid_t gid = GroupNameToGid(groupname, errc);

      if (!errc) {
        vector_list.push_back(gid);
      }

      slist.erase(0, kommapos + 1);
    }
  } while (kommapos != STR_NPOS);
}

// -----------------------------------------------------------------------------
//! Check if a vector contains uid
// -----------------------------------------------------------------------------

bool Mapping::HasUid(uid_t uid, uid_vector vector)
{
  uid_vector::const_iterator it;

  for (it = vector.begin(); it != vector.end(); ++it) {
    if ((*it) == uid) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
//! Check if vector contains gid
// -----------------------------------------------------------------------------

bool Mapping::HasGid(gid_t gid, gid_vector vector)
{
  uid_vector::const_iterator it;

  for (it = vector.begin(); it != vector.end(); ++it) {
    if ((*it) == gid) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
//! Compare a uid with the string representation
// -----------------------------------------------------------------------------

bool Mapping::IsUid(XrdOucString idstring, uid_t& id)
{
  id = strtoul(idstring.c_str(), 0, 10);
  char revid[1024];
  sprintf(revid, "%lu", (unsigned long) id);
  XrdOucString srevid = revid;

  if (idstring == srevid) {
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
//! Compare a gid with the string representation
// -----------------------------------------------------------------------------

bool Mapping::IsGid(XrdOucString idstring, gid_t& id)
{
  id = strtoul(idstring.c_str(), 0, 10);
  char revid[1024];
  sprintf(revid, "%lu", (unsigned long) id);
  XrdOucString srevid = revid;

  if (idstring == srevid) {
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
//! Reduce the trace identifier information to user@host
// -----------------------------------------------------------------------------

const char* Mapping::ReduceTident(XrdOucString& tident,
                                  XrdOucString& wildcardtident, XrdOucString& mytident, XrdOucString& myhost)
{
  int dotpos = tident.find(".");
  int addpos = tident.find("@");
  wildcardtident = tident;
  mytident = tident;
  mytident.erase(dotpos, addpos - dotpos);
  myhost = mytident;
  dotpos = mytident.find("@");
  myhost.erase(0, dotpos + 1);
  wildcardtident = mytident;
  addpos = wildcardtident.find("@");
  wildcardtident.erase(0, addpos);
  wildcardtident = "*" + wildcardtident;
  return mytident.c_str();
}

// -----------------------------------------------------------------------------
//! Convert a uid into a string
// -----------------------------------------------------------------------------

std::string Mapping::UidAsString(uid_t uid)
{
  std::string uidstring = "";
  char suid[1024];
  snprintf(suid, sizeof(suid) - 1, "%u", uid);
  uidstring = suid;
  return uidstring;
}

// -----------------------------------------------------------------------------
//! Convert a gid into a string
// -----------------------------------------------------------------------------

std::string Mapping::GidAsString(gid_t gid)
{
  std::string gidstring = "";
  char sgid[1024];
  snprintf(sgid, sizeof(sgid) - 1, "%u", gid);
  gidstring = sgid;
  return gidstring;
}

// -----------------------------------------------------------------------------
//! Copy function for virtual identities
// -----------------------------------------------------------------------------

void Mapping::Copy(Mapping::VirtualIdentity& vidin,
                   Mapping::VirtualIdentity& vidout)
{
  vidout.uid = vidin.uid;
  vidout.gid = vidin.gid;
  vidout.sudoer = vidin.sudoer;
  vidout.name = vidin.name;
  vidout.tident = vidin.tident;
  vidout.prot = vidin.prot;
  vidout.uid_list.clear();
  vidout.gid_list.clear();
  vidout.uid_string = vidin.uid_string;
  vidout.gid_string = vidin.gid_string;

  for (unsigned int i = 0; i < vidin.uid_list.size(); i++) {
    vidout.uid_list.push_back(vidin.uid_list[i]);
  }

  for (unsigned int i = 0; i < vidin.gid_list.size(); i++) {
    vidout.gid_list.push_back(vidin.gid_list[i]);
  }

  vidout.host = vidin.host;
  vidout.domain = vidin.domain;
  vidout.grps = vidin.grps;
  vidout.role = vidin.role;
  vidout.dn = vidin.dn;
  vidout.geolocation = vidin.geolocation;
  vidout.app = vidin.app;
}

//------------------------------------------------------------------------------
//! Function converting vid frin a string representation
//------------------------------------------------------------------------------

bool Mapping::VidFromString(Mapping::VirtualIdentity& vid,
                            const char* vidstring)
{
  std::string svid = vidstring;
  std::vector<std::string> tokens;
  eos::common::StringConversion::EmptyTokenize(
    vidstring,
    tokens,
    ":");

  if (tokens.size() != 7) {
    return false;
  }

  vid.uid = strtoul(tokens[0].c_str(), 0, 10);
  vid.gid = strtoul(tokens[1].c_str(), 0, 10);
  vid.uid_string = tokens[2].c_str();
  vid.gid_string = tokens[3].c_str();
  vid.name = tokens[4].c_str();
  vid.prot = tokens[5].c_str();
  vid.tident = tokens[6].c_str();
  return true;
}

//----------------------------------------------------------------------------
//! Function converting vid to a string representation
//----------------------------------------------------------------------------

std::string Mapping::VidToString(VirtualIdentity& vid)
{
  char vids[4096];
  snprintf(vids, sizeof(vids), "%u:%u:%s:%s:%s:%s:%s",
           vid.uid,
           vid.gid,
           vid.uid_string.c_str(),
           vid.gid_string.c_str(),
           vid.name.c_str(),
           vid.prot.c_str(),
           vid.tident.c_str());
  return std::string(vids);
}

//------------------------------------------------------------------------------
//! Function checking if we come from a localhost connection
//------------------------------------------------------------------------------

bool Mapping::IsLocalhost(VirtualIdentity& vid)
{
  if ((vid.host == "localhost") ||
      (vid.host == "localhost.localdomain") ||
      (vid.host == "localhost6") ||
      (vid.host == "localhost6.localdomain6")) {
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------------
//! Check for a role in the user id list
// -----------------------------------------------------------------------------

bool Mapping::HasUid(uid_t uid, VirtualIdentity& vid)
{
  for (size_t i = 0; i < vid.uid_list.size(); i++)
    if (vid.uid_list[i] == uid) {
      return true;
    }

  return false;
}

// -----------------------------------------------------------------------------
//! Check for a role in the group id list
// -----------------------------------------------------------------------------

bool Mapping::HasGid(gid_t gid, VirtualIdentity& vid)
{
  for (size_t i = 0; i < vid.gid_list.size(); i++)
    if (vid.gid_list[i] == gid) {
      return true;
    }

  return false;
}

//------------------------------------------------------------------------------
//! Function creating the Nobody identity
//------------------------------------------------------------------------------

void Mapping::Nobody(VirtualIdentity& vid)
{
  vid.uid = vid.gid = 99;
  vid.uid_list.clear();
  vid.gid_list.clear();
  vid.uid_list.push_back(99);
  vid.gid_list.push_back(99);
  vid.name = "nobody";
  vid.sudoer = false;
  vid.tident = "nobody@unknown";
}

//----------------------------------------------------------------------------
//! Function creating the root identity
//----------------------------------------------------------------------------

void Mapping::Root(VirtualIdentity& vid)
{
  vid.uid = vid.gid = 0;
  vid.uid_list.clear();
  vid.gid_list.clear();
  vid.uid_list.push_back(0);
  vid.gid_list.push_back(0);
  vid.name = "root";
  vid.prot = "local";
  vid.tident = "service@localhost";
  vid.sudoer = false;
}

/*----------------------------------------------------------------------------*/
EOSCOMMONNAMESPACE_END
