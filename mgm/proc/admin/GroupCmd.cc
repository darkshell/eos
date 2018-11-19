//------------------------------------------------------------------------------
// File: GroupCmd.cc
// Author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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

#include "GroupCmd.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/FsView.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behavior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
GroupCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::GroupProto group = mReqProto.group();
  eos::console::GroupProto::SubcmdCase subcmd = group.subcmd_case();

  if (subcmd == eos::console::GroupProto::kLs) {
    LsSubcmd(group.ls(), reply);
  } else if (subcmd == eos::console::GroupProto::kRm) {
    RmSubcmd(group.rm(), reply);
  } else if (subcmd == eos::console::GroupProto::kSet) {
    SetSubcmd(group.set(), reply);
  } else {
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

//------------------------------------------------------------------------------
// Execute ls subcommand
//------------------------------------------------------------------------------
int
GroupCmd::LsSubcmd(const eos::console::GroupProto_LsProto& ls,
                   eos::console::ReplyProto& reply)
{
  std::string format;
  std::string mListFormat;
  std::string mOutFormat; // #TOCK what for?

  switch (ls.outformat()) {
  case eos::console::GroupProto_LsProto::MONITORING:
    format = FsView::GetGroupFormat("m");
    break;

  case eos::console::GroupProto_LsProto::IOGROUP:
    format = FsView::GetGroupFormat("io");
    break;

  case eos::console::GroupProto_LsProto::IOFS: // #TOCK is having ::IOFS meaningful? it falls back to ::IOGROUP anyway
    format = FsView::GetGroupFormat("io");
    mListFormat = FsView::GetFileSystemFormat("io");
    //ls.set_outformat(eos::console::GroupProto_LsProto::IOGROUP); // #TOCK it was mOutFormat="io", but then mOutFormat never used again apparently
    break;

  case eos::console::GroupProto_LsProto::LONGER:
    format = FsView::GetGroupFormat("l");
    mListFormat = FsView::GetFileSystemFormat("l");
    break;

  default :
    // NONE
    stdErr = "error: illegal parameter 'outformat'";
    retc = EINVAL;
    break;
  }

  // if not( -b || --brief )
  if (!ls.outhost()) {
    if (format.find("S") != std::string::npos) {
      format.replace(format.find("S"), 1, "s");
    }

    if (mListFormat.find("S") != std::string::npos) {
      mListFormat.replace(mListFormat.find("S"), 1, "s");
    }
  }

  std::string output;
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  FsView::gFsView.PrintGroups(output, format, mListFormat, ls.outdepth(),
                              ls.selection().c_str());
  stdOut += output.c_str();
  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK;
}

//------------------------------------------------------------------------------
// Execute rm subcommand
//------------------------------------------------------------------------------
int
GroupCmd::RmSubcmd(const eos::console::GroupProto_RmProto& rm,
                   eos::console::ReplyProto& reply)
{
  if (mVid.uid == 0) {
    if (!rm.group().length()) {
      stdErr = "error: illegal parameter 'group'";
      retc = EINVAL;
    } else {
      eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

      if (!FsView::gFsView.mGroupView.count(rm.group())) {
        stdErr = ("error: no such group '" + rm.group() + "'").c_str();
        retc = ENOENT;
      } else {
        for (auto it = FsView::gFsView.mGroupView[rm.group()]->begin();
             it != FsView::gFsView.mGroupView[rm.group()]->end(); it++) {
          if (FsView::gFsView.mIdView.count(*it)) {
            FileSystem* fs = FsView::gFsView.mIdView[*it];

            if (fs) {
              // check that all filesystems are empty
              if ((fs->GetConfigStatus(false) != eos::common::FileSystem::kEmpty)) {
                reply.set_std_err("error: unable to remove group '" + rm.group() +
                                  "' - filesystems are not all in empty state - try list the group and drain them or set: fs config <fsid> configstatus=empty\n");
                reply.set_retc(EBUSY);
                return SFS_OK;
              }
            }
          }
        }

        std::string groupconfigname =
          eos::common::GlobalConfig::gConfig.QueuePrefixName(
            FsGroup::sGetConfigQueuePrefix(), rm.group().c_str());

        if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(
              groupconfigname.c_str())) {
          stdErr = ("error: unable to remove config of group '" + rm.group() +
                    "'").c_str();
          retc = EIO;
        } else {
          if (FsView::gFsView.UnRegisterGroup(rm.group().c_str())) {
            stdOut = ("success: removed group '" + rm.group() + "'").c_str();
          } else {
            stdErr = ("error: unable to unregister group '" + rm.group() + "'").c_str();
          }
        }
      }
    }
  } else {
    stdErr = "error: you have to take role 'root' to execute this command";
    retc = EPERM;
  }

  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK;
}

//------------------------------------------------------------------------------
// Execute set subcommand
//------------------------------------------------------------------------------
int
GroupCmd::SetSubcmd(const eos::console::GroupProto_SetProto& set,
                    eos::console::ReplyProto& reply)
{
  if (mVid.uid == 0) {
    std::string status = (set.group_state() ? "on" : "off");
    std::string key = "status";

    if (!set.group().length() || (status == "off")) {
      stdErr = "error: illegal parameters 'group/group-state'";
      retc = EINVAL;
    } else {
      eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

      if (!FsView::gFsView.mGroupView.count(set.group().c_str())) {
        stdOut = ("info: creating group '" + set.group() + "'").c_str();

        if (!FsView::gFsView.RegisterGroup(set.group().c_str())) {
          std::string groupconfigname =
            eos::common::GlobalConfig::gConfig.QueuePrefixName(
              gOFS->GroupConfigQueuePrefix.c_str(), set.group().c_str());
          retc = EIO;
          stdErr = ("error: cannot register group <" + set.group() + ">").c_str();
        }
      }

      if (!retc) {
        // Set this new group to offline
        if (!FsView::gFsView.mGroupView[set.group()]->SetConfigMember
            (key, status, true, "/eos/*/mgm")) {
          stdErr = "error: cannot set config status";
          retc = EIO;
        }

        if (status == "on") {
          // Recompute the drain status in this group
          bool setactive = false;
          FileSystem* fs = 0;

          if (FsView::gFsView.mGroupView.count(set.group())) {
            for (auto git = FsView::gFsView.mGroupView[set.group()]->begin();
                 git != FsView::gFsView.mGroupView[set.group()]->end(); git++) {
              if (FsView::gFsView.mIdView.count(*git)) {
                int drainstatus = (eos::common::FileSystem::GetDrainStatusFromString(
                                     FsView::gFsView.mIdView[*git]->GetString("drainstatus").c_str()));

                if ((drainstatus == eos::common::FileSystem::kDraining) ||
                    (drainstatus == eos::common::FileSystem::kDrainStalling)) {
                  // If any group filesystem is draining, all the others have
                  // to enable the pull for draining!
                  setactive = true;
                }
              }
            }

            for (auto git = FsView::gFsView.mGroupView[set.group()]->begin();
                 git != FsView::gFsView.mGroupView[set.group()]->end(); git++) {
              fs = FsView::gFsView.mIdView[*git];

              if (fs) {
                if (setactive) {
                  if (fs->GetString("stat.drainer") != "on") {
                    fs->SetString("stat.drainer", "on");
                  }
                } else {
                  if (fs->GetString("stat.drainer") != "off") {
                    fs->SetString("stat.drainer", "off");
                  }
                }
              }
            }
          }
        }

        if (status == "off") {
          // Disable all draining in this group
          FileSystem* fs = 0;

          if (FsView::gFsView.mGroupView.count(set.group())) {
            for (auto git = FsView::gFsView.mGroupView[set.group()]->begin();
                 git != FsView::gFsView.mGroupView[set.group()]->end(); git++) {
              fs = FsView::gFsView.mIdView[*git];

              if (fs) {
                fs->SetString("stat.drainer", "off");
              }
            }
          }
        }
      }
    }
  } else {
    retc = EPERM;
    stdErr = "error: you have to take role 'root' to execute this command";
  }

  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK; // #TOCK is this needed?
}

EOSMGMNAMESPACE_END
