//------------------------------------------------------------------------------
//! @file eosfuse.cc
//! @author Andreas-Joachim Peters CERN
//! @brief EOS C++ Fuse low-level implementation (3rd generation)
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016CERN/Switzerland                                  *
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

#include "common/StacktraceHere.hh"
#ifndef __APPLE__
#include "common/ShellCmd.hh"
#endif
#ifdef ROCKSDB_FOUND
#include "kv/RocksKV.hh"
#endif

#include "eosfuse.hh"
#include "misc/fusexrdlogin.hh"
#include "misc/filename.hh"
#include <string>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <memory>
#include <algorithm>
#include <thread>
#include <iterator>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#ifdef RICHACL_FOUND
extern "C" { /* this 'extern "C"' brace will eventually end up in the .h file, then it can be removed */
#include <sys/richacl.h>
}
#include "misc/richacl.hh"
#endif

#include <sys/resource.h>
#include <sys/types.h>
#include "common/XattrCompat.hh"

#ifdef __APPLE__
#define O_DIRECT 0
#define EKEYEXPIRED 127
#else
#include <sys/resource.h>
#endif

#include <json/json.h>
#include "common/Timing.hh"
#include "common/Logging.hh"
#include "common/Path.hh"
#include "common/LinuxMemConsumption.hh"
#include "common/LinuxStat.hh"
#include "common/StringConversion.hh"
#include "md/md.hh"
#include "md/kernelcache.hh"
#include "kv/kv.hh"
#include "data/cache.hh"
#include "data/cachehandler.hh"

#if ( FUSE_USE_VERSION > 28 )
#include "EosFuseSessionLoop.hh"
#endif

#define _FILE_OFFSET_BITS 64

const char* k_mdino = "sys.eos.mdino";
const char* k_nlink = "sys.eos.nlink";
const char* k_fifo = "sys.eos.fifo";
EosFuse* EosFuse::sEosFuse = 0;

/* -------------------------------------------------------------------------- */
EosFuse::EosFuse()
{
  sEosFuse = this;
  fusesession = 0;
  fusechan = 0;
}

/* -------------------------------------------------------------------------- */
EosFuse::~EosFuse()
{
}

/* -------------------------------------------------------------------------- */
int
/* -------------------------------------------------------------------------- */
EosFuse::run(int argc, char* argv[], void* userdata)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  env->PutInt("RunForkHandler", 1);
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  fuse_opt_parse(&args, NULL, NULL, NULL);
  char* local_mount_dir = 0;
  int err = 0;
  std::string no_fsync_list;
  // check the fsname to choose the right JSON config file
  std::string fsname = "";

  for (int i = 0; i < argc; i++) {
    std::string option = argv[i];
    size_t npos;
    size_t epos;

    if ((npos = option.find("fsname=")) != std::string::npos) {
      epos = option.find(",", npos);
      fsname = option.substr(npos + std::string("fsname=").length(),
			     (epos != std::string::npos) ?
			     epos - npos - std::string("fsname=").length() : -1);
      break;
    }
  }

  fprintf(stderr, "# fsname='%s'\n", fsname.c_str());

  if (getuid() == 0) {
    // the root mount always adds the 'allow_other' option
    fuse_opt_add_arg(&args, "-oallow_other");
    fprintf(stderr, "# -o allow_other enabled on shared mount\n");
  }

  fprintf(stderr, "# -o big_writes enabled\n");
  fuse_opt_add_arg(&args, "-obig_writes");
  std::string jsonconfig = "/etc/eos/fuse";

  if (geteuid()) {
    jsonconfig = getenv("HOME");
    jsonconfig += "/.eos/fuse";
  }

  if (fsname.length()) {
    if (((fsname.find("@") == std::string::npos)) &&
	((fsname.find(":") == std::string::npos))) {
      jsonconfig += ".";
      jsonconfig += fsname;
    }
  }

  jsonconfig += ".conf";
#ifndef __APPLE__

  if (::access("/bin/fusermount", X_OK)) {
    fprintf(stderr, "error: /bin/fusermount is not executable for you!\n");
    exit(-1);
  }

#endif

  if (getuid() == 0) {
    unsetenv("KRB5CCNAME");
    unsetenv("X509_USER_PROXY");
  }

  cacheconfig cconfig;
  // ---------------------------------------------------------------------------------------------
  // The logic of configuration works liks that:
  // - every configuration value has a corresponding default value
  // - the configuration file name is taken from the fsname option given on the command line
  //   e.g. root> eosxd -ofsname=foo loads /etc/eos/fuse.foo.conf
  //        root> eosxd              loads /etc/eos/fuse.conf
  //        user> eosxd -ofsname=foo loads $HOME/.eos/fuse.foo.conf
  // One can avoid to use configuration files if the defaults are fine providing the remote host and remote mount directory via the fsname
  //   e.g. root> eosxd -ofsname=eos.cern.ch:/eos/ $HOME/eos mounts the /eos/ directory from eos.cern.ch shared under $HOME/eos/
  //   e.g. user> eosxd -ofsname=user@eos.cern.ch:/eos/user/u/user/ $home/eos mounts /eos/user/u/user from eos.cern.ch private under $HOME/eos/
  //   If this is a user-private mount the syntax 'foo@cern.ch' should be used to distinguish private mounts of individual users in the 'df' output
  //
  //   Please note, that root mounts are by default shared mounts with kerberos configuration,
  //   user mounts are private mounts with kerberos configuration
  // --------------------------------------------------------------------------------------------
  // XrdCl::* options we read from our config file
  std::vector<std::string> xrdcl_options;
  xrdcl_options.push_back("TimeoutResolution");
  xrdcl_options.push_back("ConnectionWindow");
  xrdcl_options.push_back("ConnectionRetry");
  xrdcl_options.push_back("StreamErrorWindow");
  xrdcl_options.push_back("RequestTimeout");
  xrdcl_options.push_back("StreamTimeout");
  xrdcl_options.push_back("RedirectLimit");
  {
    // parse JSON configuration
    Json::Value root;
    Json::Reader reader;
    struct stat configstat;
    bool has_config = false;

    if (!::stat(jsonconfig.c_str(), &configstat)) {
      std::ifstream configfile(jsonconfig, std::ifstream::binary);

      if (reader.parse(configfile, root, false)) {
	fprintf(stderr, "# JSON parsing successfull\n");
	has_config = true;
      } else {
	fprintf(stderr, "error: invalid configuration file %s - %s\n",
		jsonconfig.c_str(), reader.getFormattedErrorMessages().c_str());
	exit(EINVAL);
      }
    } else {
      fprintf(stderr, "# no config file - running on default values\n");
    }

    if (!root.isMember("hostport")) {
      if (has_config) {
	fprintf(stderr,
		"error: please configure 'hostport' in your configuration file '%s'\n",
		jsonconfig.c_str());
	exit(EINVAL);
      }

      if (!fsname.length()) {
	fprintf(stderr,
		"error: please configure the EOS endpoint via fsname=<user>@<host\n");
	exit(EINVAL);
      }

      if ((fsname.find(".") == std::string::npos)) {
	fprintf(stderr,
		"error: when running without a configuration file you need to configure the EOS endpoint via fsname=<host>.<domain> - the domain has to be added!\n");
	exit(EINVAL);
      }

      size_t pos_add;

      if ((pos_add = fsname.find("@")) != std::string::npos) {
	std::string fsuser = fsname;
	fsname.erase(0, pos_add + 1);
	fsuser.erase(pos_add);

	if ((fsuser == "gw") || (fsuser == "smb")) {
	  // keep always all meta-data
	  root["options"]["free-md-asap"] = 0;

	  // if 'gw' = gateway is defined as user name, we enable stable inode support e.g. mdcachedir
	  if (!root.isMember("mdcachedir")) {
	    if (geteuid()) {
	      root["mdcachedir"] = "/var/tmp/eos/fusex/md-cache/";
	    } else {
	      root["mdcachedir"] = "/var/cache/eos/fusex/md-cache/";
	    }

	    fprintf(stderr, "# enabling stable inodes with md-cache in '%s'\n",
		    root["mdcachedir"].asString().c_str());
	  }

	  root["auth"]["krb5"] = 0;

	  if (fsuser == "smb") {
	    // enable overlay mode
	    if (!root["options"].isMember("overlay-mode")) {
	      root["options"]["overlay-mode"] = "0777";
	      fprintf(stderr, "# enabling overlay-mode 0777 for smb export\n");
	    }
	  }
	}
      }

      size_t pos_colon;

      if ((pos_colon = fsname.find(":")) != std::string::npos) {
	std::string remotemount = fsname.substr(pos_colon + 1);
	fsname.erase(pos_colon);
	root["remotemountdir"] = remotemount;
	fprintf(stderr, "# extracted remote mount dir from fsname is '%s'\n",
		remotemount.c_str());
      }

      root["hostport"] = fsname;
      fprintf(stderr, "# extracted connection host from fsname is '%s'\n",
	      fsname.c_str());
    }

    // apply some default settings for undefined entries.
    {
      if (!root.isMember("name")) {
	root["name"] = "";
      }

      if (!root.isMember("hostport")) {
	root["hostport"] = "localhost";
      }

      if (!root.isMember("mdzmqidentity")) {
	if (geteuid()) {
	  root["mdzmqidentity"] = "userd";
	} else {
	  root["mdzmqidentity"] = "eosxd";
	}
      }

      if (!root.isMember("remotemountdir")) {
	root["remotemountdir"] = "/eos/";
      }

      if (!root.isMember("localmountdir")) {
	root["localmountdir"] = "/eos/";
      }

      if (!root["options"].isMember("debuglevel")) {
	root["options"]["debuglevel"] = 4;
      }

      if (!root["options"].isMember("backtrace")) {
	root["options"]["backtrace"] = 1;
      }

      if (!root["options"].isMember("md-kernelcache")) {
	root["options"]["md-kernelcache"] = 1;
      }

      if (!root["options"].isMember("md-kernelcache.enoent.timeout")) {
	root["options"]["md-kernelcache.enoent.timeout"] = 0.01;
      }

      if (!root["options"].isMember("md-backend.timeout")) {
	root["options"]["md-backend.timeout"] = 86400;
      }

      if (!root["options"].isMember("md-backend.put.timeout")) {
	root["options"]["md-backend.put.timeout"] = 120;
      }

      if (!root["options"].isMember("data-kernelcache")) {
	root["options"]["data-kernelcache"] = 1;
      }

      if (!root["options"].isMember("mkdir-is-sync")) {
	root["options"]["mkdir-is-sync"] = 1;
      }

      if (!root["options"].isMember("create-is-sync")) {
	root["options"]["create-is-sync"] = 1;
      }

      if (!root["options"].isMember("symlink-is-sync")) {
	root["options"]["symlink-is-sync"] = 1;
      }

      if (!root["options"].isMember("rename-is-sync")) {
	root["options"]["rename-is-sync"] = 1;
      }

      if (!root["options"].isMember("rm-is-sync")) {
	root["options"]["rm-is-sync"] = 0;
      }

      if (!root["options"].isMember("global-flush")) {
	root["options"]["global-flush"] = 1;
      }

      if (!root["options"].isMember("global-locking")) {
	root["options"]["global-locking"] = 1;
      }

      if (!root["options"].isMember("flush-wait-open")) {
	root["options"]["flush-wait-open"] = 1;
      }

      if (!root["options"].isMember("show-tree-size")) {
	root["options"]["show-tree-size"] = 0;
      }

      if (!root["options"].isMember("free-md-asap")) {
	root["options"]["free-md-asap"] = 1;
      }

      if (!root["auth"].isMember("krb5")) {
	root["auth"]["krb5"] = 1;
      }

      if (!root["inline"].isMember("max-size")) {
	root["inline"]["max-size="] = 0;
      }

      if (!root["inline"].isMember("default-compressor")) {
	root["inline"]["default-compressor"] = "none";
      }

      if (!root["auth"].isMember("shared-mount")) {
	if (geteuid()) {
	  root["auth"]["shared-mount"] = 0;
	} else {
	  root["auth"]["shared-mount"] = 1;
	}
      }

      if (!root["options"].isMember("fd-limit")) {
	if (!geteuid()) {
	  root["options"]["fd-limit"] = 65535;
	} else {
	  root["options"]["fd-limit"] = 4096;
	}
      }

      if (!root["options"].isMember("no-fsync")) {
	root["options"]["no-fsync"].append(".db");
	root["options"]["no-fsync"].append(".db-journal");
	root["options"]["no-fsync"].append(".sqlite");
	root["options"]["no-fsync"].append(".sqlite-journal");
	root["options"]["no-fsync"].append(".db3");
	root["options"]["no-fsync"].append(".db3-journal");
	root["options"]["no-fsync"].append("*.o");
      }
    }

    if (!root["options"].isMember("cpu-core-affinity")) {
      root["options"]["cpu-core-affinity"] = 1;
    }

    if (!root["options"].isMember("no-xattr")) {
      root["options"]["no-xattr"] = 0;
    }

    if (!root["options"].isMember("no-link")) {
      root["options"]["no-link"] = 1;
    }

    if (!root["options"].isMember("nocache-graceperiod")) {
      root["options"]["nocache-graceperiod"] = 5;
    }

    if (!root["auth"].isMember("forknoexec-heuristic")) {
      root["auth"]["forknoexec-heuristic"] = 1;
    }

    if (!root["options"].isMember("rm-rf-protect-levels")) {
      root["options"]["rm-rf-protect-levels"] = 1;
    }

    if (!root["options"].isMember("rm-rf-bulk")) {
      root["options"]["rm-rf-bulk"] = 0;
    }

    // xrdcl default options
    XrdCl::DefaultEnv::GetEnv()->PutInt("TimeoutResolution", 1);
    XrdCl::DefaultEnv::GetEnv()->PutInt("ConnectionWindow", 10);
    XrdCl::DefaultEnv::GetEnv()->PutInt("ConnectionRetry", 0);
    XrdCl::DefaultEnv::GetEnv()->PutInt("StreamErrorWindow", 60);
    XrdCl::DefaultEnv::GetEnv()->PutInt("RequestTimeout", 30);
    XrdCl::DefaultEnv::GetEnv()->PutInt("StreamTimeout", 60);
    XrdCl::DefaultEnv::GetEnv()->PutInt("RedirectLimit", 3);

    for (auto it = xrdcl_options.begin(); it != xrdcl_options.end(); ++it) {
      if (root["xrdcl"].isMember(*it)) {
	XrdCl::DefaultEnv::GetEnv()->PutInt(it->c_str(),
					    root["xrdcl"][it->c_str()].asInt());

	if (*it == "RequestTimeout") {
	  int rtimeout = root["xrdcl"][it->c_str()].asInt();

	  if (rtimeout > XrdCl::Proxy::chunk_timeout()) {
	    XrdCl::Proxy::chunk_timeout(rtimeout + 60);
	  }
	}
      }
    }

    if (root["xrdcl"].isMember("LogLevel")) {
      XrdCl::DefaultEnv::GetEnv()->PutString("LogLevel",
					     root["xrdcl"]["LogLevel"].asString());
      setenv((char*) "XRD_LOGLEVEL", root["xrdcl"]["LogLevel"].asString().c_str(), 1);
      XrdCl::DefaultEnv::ReInitializeLogging();
    }

    // recovery setting
    if (!root["recovery"].isMember("read")) {
      root["recovery"]["read"] = 1;
    }

    if (!root["recovery"].isMember("read-open")) {
      root["recovery"]["read-open"] = 1;
    }

    if (!root["recovery"].isMember("read-open-noserver")) {
      root["recovery"]["read-open-noserver"] = 1;
    }

    if (!root["recovery"].isMember("read-open-noserver-retrywindow")) {
      root["recovery"]["read-open-noserver-retrywindow"] = 86400;
    }

    if (!root["recovery"].isMember("write")) {
      root["recovery"]["write"] = 1;
    }

    if (!root["recovery"].isMember("write-open")) {
      root["recovery"]["write-open"] = 1;
    }

    if (!root["recovery"].isMember("write-open-noserver")) {
      root["recovery"]["write-open-noserver"] = 1;
    }

    if (!root["recovery"].isMember("write-open-noserver-retrywindow")) {
      root["recovery"]["write-open-noserver-retrywindow"] = 86400;
    }

    const Json::Value jname = root["name"];
    config.name = root["name"].asString();
    config.hostport = root["hostport"].asString();
    config.remotemountdir = root["remotemountdir"].asString();
    config.localmountdir = root["localmountdir"].asString();
    config.statfilesuffix = root["statfilesuffix"].asString();
    config.statfilepath = root["statfilepath"].asString();
    config.options.debug = root["options"]["debug"].asInt();
    config.options.debuglevel = root["options"]["debuglevel"].asInt();
    config.options.enable_backtrace = root["options"]["backtrace"].asInt();
    config.options.libfusethreads = root["options"]["libfusethreads"].asInt();
    config.options.md_kernelcache = root["options"]["md-kernelcache"].asInt();
    config.options.md_kernelcache_enoent_timeout =
      root["options"]["md-kernelcache.enoent.timeout"].asDouble();
    config.options.md_backend_timeout =
      root["options"]["md-backend.timeout"].asDouble();
    config.options.md_backend_put_timeout =
      root["options"]["md-backend.put.timeout"].asDouble();
    config.options.data_kernelcache = root["options"]["data-kernelcache"].asInt();
    config.options.mkdir_is_sync = root["options"]["mkdir-is-sync"].asInt();
    config.options.create_is_sync = root["options"]["create-is-sync"].asInt();
    config.options.symlink_is_sync = root["options"]["symlink-is-sync"].asInt();
    config.options.rename_is_sync = root["options"]["rename-is-sync"].asInt();
    config.options.rmdir_is_sync = root["options"]["rmdir-is-sync"].asInt();
    config.options.global_flush = root["options"]["global-flush"].asInt();
    config.options.flush_wait_open = root["options"]["flush-wait-open"].asInt();
    config.options.global_locking = root["options"]["global-locking"].asInt();
    config.options.overlay_mode = strtol(
				    root["options"]["overlay-mode"].asString().c_str(), 0, 8);
    config.options.fdlimit = root["options"]["fd-limit"].asInt();
    config.options.rm_rf_protect_levels =
      root["options"]["rm-rf-protect-levels"].asInt();
    config.options.rm_rf_bulk =
      root["options"]["rm-rf-bulk"].asInt();
    config.options.show_tree_size = root["options"]["show-tree-size"].asInt();
    config.options.free_md_asap = root["options"]["free-md-asap"].asInt();
    config.options.cpu_core_affinity = root["options"]["cpu-core-affinity"].asInt();
    config.options.no_xattr = root["options"]["no-xattr"].asInt();
    config.options.no_hardlinks = root["options"]["no-link"].asInt();

    if (config.options.no_xattr) {
      disable_xattr();
    }

    if (config.options.no_hardlinks) {
      disable_link();
    }

    config.options.nocache_graceperiod =
      root["options"]["nocache-graceperiod"].asInt();
    config.recovery.read = root["recovery"]["read"].asInt();
    config.recovery.read_open = root["recovery"]["read-open"].asInt();
    config.recovery.read_open_noserver =
      root["recovery"]["read-open-noserver"].asInt();
    config.recovery.read_open_noserver_retrywindow =
      root["recovery"]["read-open-noserver-retrywindow"].asInt();
    config.recovery.write = root["recovery"]["write"].asInt();
    config.recovery.write_open = root["recovery"]["write-open"].asInt();
    config.recovery.write_open_noserver =
      root["recovery"]["write-open-noserver"].asInt();
    config.recovery.write_open_noserver_retrywindow =
      root["recovery"]["write-open-noserver-retrywindow"].asInt();
    config.mdcachehost = root["mdcachehost"].asString();
    config.mdcacheport = root["mdcacheport"].asInt();
    config.mdcachedir = root["mdcachedir"].asString();
    config.mqtargethost = root["mdzmqtarget"].asString();
    config.mqidentity = root["mdzmqidentity"].asString();
    config.mqname = config.mqidentity;
    config.auth.fuse_shared = root["auth"]["shared-mount"].asInt();
    config.auth.use_user_krb5cc = root["auth"]["krb5"].asInt();
    config.auth.use_user_gsiproxy = root["auth"]["gsi"].asInt();
    config.auth.tryKrb5First = !((bool)root["auth"]["gsi-first"].asInt());
    config.auth.environ_deadlock_timeout =
      root["auth"]["environ-deadlock-timeout"].asInt();
    config.auth.forknoexec_heuristic = root["auth"]["forknoexec-heuristic"].asInt();

    if (config.auth.environ_deadlock_timeout <= 0) {
      config.auth.environ_deadlock_timeout = 100;
    }

    config.inliner.max_size = root["inline"]["max-size"].asInt();
    config.inliner.default_compressor =
      root["inline"]["default-compressor"].asString();

    if ((config.inliner.default_compressor != "none") &&
	(config.inliner.default_compressor != "zlib")) {
      std::cerr <<
		"inline default compressor value can only be 'none' or 'zlib'."
		<< std::endl;
      exit(EINVAL);
    }

    for (Json::Value::iterator it = root["options"]["no-fsync"].begin();
	 it != root["options"]["no-fsync"].end(); ++it) {
      config.options.no_fsync_suffixes.push_back(it->asString());
      no_fsync_list += it->asString();
      no_fsync_list += ",";
    }

    // disallow mdcachedir if compiled without rocksdb support
#ifndef ROCKSDB_FOUND

    if (!config.mdcachedir.empty()) {
      std::cerr <<
		"Options mdcachedir is unavailable, fusex was compiled without rocksdb support."
		<< std::endl;
      exit(EINVAL);
    }

#endif

    // disallow conflicting options
    if (!config.mdcachedir.empty() && (config.mdcacheport != 0 ||
				       !config.mdcachehost.empty())) {
      std::cerr <<
		"Options (mdcachehost, mdcacheport) conflict with (mdcachedir) - only one type of mdcache is allowed."
		<< std::endl;
      exit(EINVAL);
    }

    if (config.mdcachedir.length()) {
      // add the instance name to all cache directories
      if (config.mdcachedir.rfind("/") != (config.mdcachedir.size() - 1)) {
	config.mdcachedir += "/";
      }

      config.mdcachedir += config.name.length() ? config.name : "default";
    }

    // default settings
    if (!config.statfilesuffix.length()) {
      config.statfilesuffix = "stats";
    }

    if (!config.mdcacheport) {
      config.mdcacheport = 6379;
    }

    if (!config.mqtargethost.length()) {
      std::string h = config.hostport;

      if (h.find(":") != std::string::npos) {
	h.erase(h.find(":"));
      }

      config.mqtargethost = "tcp://" + h + ":1100";
    }

    {
      config.mqidentity.insert(0, "fuse://");
      config.mqidentity += "@";
      char hostname[4096];

      if (gethostname(hostname, sizeof(hostname))) {
	fprintf(stderr, "error: failed to get hostname!\n");
	exit(EINVAL);
      }

      config.clienthost = hostname;
      config.mqidentity += hostname;
      char suuid[40];
      uuid_t uuid;
      uuid_generate_time(uuid);
      uuid_unparse(uuid, suuid);
      config.clientuuid = suuid;
      config.mqidentity += "//";
      config.mqidentity += suuid;
      config.mqidentity += ":";
      char spid[16];
      snprintf(spid, sizeof(spid), "%d", getpid());
      config.mqidentity += spid;
    }

    if (config.options.fdlimit > 0) {
      struct rlimit newrlimit;
      newrlimit.rlim_cur = config.options.fdlimit;
      newrlimit.rlim_max = config.options.fdlimit;

      if ((setrlimit(RLIMIT_NOFILE, &newrlimit) != 0) && (!geteuid())) {
	fprintf(stderr, "warning: unable to set fd limit to %lu - errno %d\n",
		config.options.fdlimit, errno);
      }
    }

    struct rlimit nofilelimit;

    if (getrlimit(RLIMIT_NOFILE, &nofilelimit) != 0) {
      fprintf(stderr, "error: unable to get fd limit - errno %d\n", errno);
      exit(EINVAL);
    }

    fprintf(stderr, "# File descriptor limit: %lu soft, %lu hard\n",
	    nofilelimit.rlim_cur, nofilelimit.rlim_max);
    // store the current limit
    config.options.fdlimit = nofilelimit.rlim_cur;
    // data caching configuration
    cconfig.type = cache_t::INVALID;

    if (!config.mdcachehost.length() && !config.mdcachedir.length()) {
      cconfig.clean_on_startup = true;
    } else {
      cconfig.clean_on_startup = false;
    }

    if (root["cache"]["type"].asString() == "disk") {
      cconfig.type = cache_t::DISK;
    } else if (root["cache"]["type"].asString() == "memory") {
      cconfig.type = cache_t::MEMORY;
    } else {
      if (root["cache"]["type"].asString().length()) {
	fprintf(stderr, "error: invalid cache type configuration\n");
	exit(EINVAL);
      } else {
	cconfig.type = cache_t::DISK;
      }
    }

    if (!root["cache"].isMember("read-ahead-bytes-nominal")) {
      root["cache"]["read-ahead-bytes-nominal"] = 256 * 1024;
    }

    if (!root["cache"].isMember("read-ahead-bytes-max")) {
      root["cache"]["read-ahead-bytes-max"] = 2 * 1024 * 1024;
    }

    if (!root["cache"].isMember("read-ahead-blocks-max")) {
      root["cache"]["read-ahead-blocks-max"] = 16;
    }

    if (!root["cache"].isMember("read-ahead-strategy")) {
      root["cache"]["read-ahead-strategy"] = "dynamic";
    }

    cconfig.location = root["cache"]["location"].asString();
    cconfig.journal = root["cache"]["journal"].asString();
    cconfig.default_read_ahead_size =
      root["cache"]["read-ahead-bytes-nominal"].asInt();
    cconfig.max_read_ahead_size = root["cache"]["read-ahead-bytes-max"].asInt();
    cconfig.max_read_ahead_blocks = root["cache"]["read-ahead-blocks-max"].asInt();
    cconfig.read_ahead_strategy = root["cache"]["read-ahead-strategy"].asString();

    if ((cconfig.read_ahead_strategy != "none") &&
	(cconfig.read_ahead_strategy != "static") &&
	(cconfig.read_ahead_strategy != "dynamic")) {
      fprintf(stderr,
	      "error: invalid read-ahead-strategy specified - only 'none' 'static' 'dynamic' allowed\n");
      exit(EINVAL);
    }

    // set defaults for journal and file-start cache
    if (geteuid()) {
      if (!cconfig.location.length()) {
	cconfig.location = "/var/tmp/eos/fusex/cache/";
	cconfig.location += getenv("USER");
	cconfig.location += "/";
      }

      if (!cconfig.journal.length()) {
	cconfig.journal = "/var/tmp/eos/fusex/cache/";
	cconfig.journal += getenv("USER");
	cconfig.journal += "/";
      }

      // default cache size 512 MB
      if (!root["cache"]["size-mb"].asString().length()) {
	root["cache"]["size-mb"] = 512;
      }

      // default cache size 64k inodes
      if (!root["cache"]["size-ino"].asString().length()) {
	root["cache"]["size-ino"] = 65536;
      }

      // default cleaning threshold
      if (!root["cache"]["clean-threshold"].asString().length()) {
	root["cache"]["clean-threshold"] = 85.0;
      }
    } else {
      if (!cconfig.location.length()) {
	cconfig.location = "/var/cache/eos/fusex/cache/";
      }

      if (!cconfig.journal.length()) {
	cconfig.journal = "/var/cache/eos/fusex/cache/";
      }

      // default cache size 1 GB
      if (!root["cache"]["size-mb"].asString().length()) {
	root["cache"]["size-mb"] = 1000;
      }

      // default cache size 64k indoes
      if (!root["cache"]["size-ino"].asString().length()) {
	root["cache"]["size-ino"] = 65536;
      }

      // default cleaning threshold
      if (!root["cache"]["clean-threshold"].asString().length()) {
	root["cache"]["clean-threshold"] = 85.0;
      }
    }

    if (cconfig.location == "OFF") {
      // disable file-start cache
      cconfig.location = "";
    }

    if (cconfig.journal == "OFF") {
      // disable journal
      cconfig.journal = "";
    }

    if (cconfig.location.length()) {
      if (cconfig.location.rfind("/") != (cconfig.location.size() - 1)) {
	cconfig.location += "/";
      }

      cconfig.location += config.name.length() ? config.name : "default";
    }

    if (cconfig.journal.length()) {
      if (cconfig.journal.rfind("/") != (cconfig.journal.size() - 1)) {
	cconfig.journal += "/";
      }

      cconfig.journal += config.name.length() ? config.name : "default";
    }

    // apply some defaults for all existing options
    // by default create all the specified cache paths
    std::string mk_cachedir = "mkdir -p " + config.mdcachedir;
    std::string mk_journaldir = "mkdir -p " + cconfig.journal;
    std::string mk_locationdir = "mkdir -p " + cconfig.location;

    if (config.mdcachedir.length()) {
      system(mk_cachedir.c_str());
    }

    if (cconfig.journal.length()) {
      system(mk_journaldir.c_str());
    }

    if (cconfig.location.length()) {
      system(mk_locationdir.c_str());
    }

    // make the cache directories private to root
    if (config.mdcachedir.length()) if ((chmod(config.mdcachedir.c_str(),
					   S_IRUSR | S_IWUSR | S_IXUSR))) {
	fprintf(stderr, "error: failed to make path=%s RWX for root - errno=%d",
		config.mdcachedir.c_str(), errno);
	exit(-1);
      }

    if (cconfig.journal.length()) if ((chmod(cconfig.journal.c_str(),
					 S_IRUSR | S_IWUSR | S_IXUSR))) {
	fprintf(stderr, "error: failed to make path=%s RWX for root - errno=%d",
		cconfig.journal.c_str(), errno);
	exit(-1);
      }

    if (cconfig.location.length()) if ((chmod(cconfig.location.c_str(),
					  S_IRUSR | S_IWUSR | S_IXUSR))) {
	fprintf(stderr, "error: failed to make path=%s RWX for root - errno=%d",
		cconfig.location.c_str(), errno);
	exit(-1);
      }

    cconfig.total_file_cache_size = root["cache"]["size-mb"].asUInt64() * 1024 *
				    1024;
    cconfig.total_file_cache_inodes = root["cache"]["size-ino"].asUInt64();
    cconfig.total_file_journal_size = root["cache"]["journal-mb"].asUInt64() *
				      1024 * 1024;
    cconfig.per_file_cache_max_size = root["cache"]["file-cache-max-kb"].asUInt64()
				      * 1024;
    cconfig.per_file_journal_max_size =
      root["cache"]["file-journal-max-kb"].asUInt64() * 1024;
    cconfig.clean_threshold = root["cache"]["clean-threshold"].asDouble();
    int rc = 0;

    if ((rc = cachehandler::instance().init(cconfig))) {
      exit(rc);
    }
  }
  {
    std::string mountpoint;

    for (int i = 1; i < argc; ++i) {
      std::string opt = argv[i];
      std::string opt0 = argv[i - 1];

      if ((opt[0] != '-') && (opt0 != "-o")) {
	mountpoint = opt;
      }

      if (opt == "-f") {
	config.options.foreground = 1;
      }
    }

    if (!mountpoint.length()) {
      // we allow to take the mountpoint from the json file if it is not given on the command line
      fuse_opt_add_arg(&args, config.localmountdir.c_str());
      mountpoint = config.localmountdir.c_str();
    } else {
      config.localmountdir = mountpoint;
    }

    if (mountpoint.length()) {
      DIR* d = 0;
      struct stat d_stat;

      // sanity check of the mount directory
      if (!(d = ::opendir(mountpoint.c_str()))) {
	// check for a broken mount
	if ((errno == ENOTCONN) || (errno == ENOENT)) {
	  // force an 'umount -l '
	  std::string systemline = "umount -l ";
	  systemline += mountpoint;
	  fprintf(stderr, "# dead mount detected - forcing '%s'\n", systemline.c_str());
	  system(systemline.c_str());
	}

	if (stat(mountpoint.c_str(), &d_stat)) {
	  if (errno == ENOENT) {
	    fprintf(stderr, "error: mountpoint '%s' does not exist\n", mountpoint.c_str());
	    exit(-1);
	  } else {
	    fprintf(stderr, "error: failed to stat '%s' - errno = %d\n", mountpoint.c_str(),
		    errno);
	    exit(-1);
	  }
	}
      } else {
	closedir(d);
      }
    }
  }
  std::string nodelay = getenv("XRD_NODELAY") ? getenv("XRD_NODELAY") : "";

  if (nodelay == "1") {
    fprintf(stderr, "# Running with XRD_NODELAY=1 (nagle algorithm is disabled)\n");
  } else {
    putenv((char*) "XRD_NODELAY=1");
    fprintf(stderr, "# Disabling nagle algorithm (XRD_NODELAY=1)\n");
  }

  if (!getenv("MALLOC_CONF")) {
    fprintf(stderr, "# Setting MALLOC_CONF=dirty_decay_ms:0\n");
    putenv((char*) "MALLOC_CONF=dirty_decay_ms:0");
  } else {
    fprintf(stderr, "# MALLOC_CONF=%s\n", getenv("MALLOC_CONF"));
  }

  int debug;

  if (fuse_parse_cmdline(&args, &local_mount_dir, NULL, &debug) == -1) {
    exit(errno ? errno : -1);
  }

  if ((fusechan = fuse_mount(local_mount_dir, &args)) == NULL) {
    fprintf(stderr, "error: fuse_mount failed\n");
    exit(errno ? errno : -1);
  }

  if (fuse_daemonize(config.options.foreground) != -1) {
#ifndef __APPLE__
    eos::common::ShellCmd cmd("echo eos::common::ShellCmd init 2>&1");
    eos::common::cmd_status st = cmd.wait(5);
    int rc = st.exit_code;

    if (rc) {
      fprintf(stderr,
	      "error: failed to run shell command\n");
      exit(-1);
    }

    if (!geteuid()) {
      // change the priority of this process to maximum
      if (setpriority(PRIO_PROCESS, getpid(), -PRIO_MAX / 2) < 0) {
	fprintf(stderr,
		"error: failed to renice this process '%u', to maximum priority '%d'\n",
		getpid(), -PRIO_MAX / 2);
      }

      if (config.options.cpu_core_affinity > 0) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(config.options.cpu_core_affinity - 1, &cpuset);
	sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset);
	fprintf(stderr, "# Setting CPU core affinity to core %d\n",
		config.options.cpu_core_affinity - 1);
      }
    }

#endif
    fusexrdlogin::initializeProcessCache(config.auth);

    if (config.options.foreground) {
      if (nodelay != "1") {
	fprintf(stderr,
		"# warning: nagle algorithm is still enabled (export XRD_NODELAY=1 before running in foreground)\n");
      }
    }

    FILE* fstderr;

    // Open log file
    if (getuid()) {
      char logfile[1024];

      if (getenv("EOS_FUSE_LOGFILE")) {
	snprintf(logfile, sizeof(logfile) - 1, "%s",
		 getenv("EOS_FUSE_LOGFILE"));
      } else {
	snprintf(logfile, sizeof(logfile) - 1, "/tmp/eos-fuse.%d.log",
		 getuid());
      }

      if (!config.statfilepath.length()) {
	config.statfilepath = logfile;
	config.statfilepath += ".";
	config.statfilepath += config.statfilesuffix;
      }

      // Running as a user ... we log into /tmp/eos-fuse.$UID.log
      if (!(fstderr = freopen(logfile, "a+", stderr))) {
	fprintf(stdout, "error: cannot open log file %s\n", logfile);
      } else {
	if (::chmod(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) {
	  fprintf(stderr, "error: cannot change permission of log file %s\n", logfile);
	  exit(-1);
	}
      }
    } else {
      // Running as root ... we log into /var/log/eos/fuse
      std::string log_path = "/var/log/eos/fusex/fuse.";

      if (getenv("EOS_FUSE_LOG_PREFIX") || fsname.length()) {
	if (getenv("EOS_FUSE_LOG_PREFIX")) {
	  log_path += getenv("EOS_FUSE_LOG_PREFIX");
	} else {
	  log_path += fsname;
	}

	if (!config.statfilepath.length()) config.statfilepath = log_path +
	      "." + config.statfilesuffix;

	log_path += ".log";
      } else {
	if (!config.statfilepath.length()) config.statfilepath = log_path +
	      config.statfilesuffix;

	log_path += "log";
      }

      eos::common::Path cPath(log_path.c_str());
      cPath.MakeParentPath(S_IRWXU | S_IRGRP | S_IROTH);

      if (!(fstderr = freopen(cPath.GetPath(), "a+", stderr))) {
	fprintf(stderr, "error: cannot open log file %s\n", cPath.GetPath());
      } else if (::chmod(cPath.GetPath(), S_IRUSR | S_IWUSR)) {
	fprintf(stderr, "error: failed to chmod %s\n", cPath.GetPath());
      }
    }

    if (fstderr) {
      setvbuf(fstderr, (char*) NULL, _IONBF, 0);
    }

#ifdef EOSCITRINE
    eos::common::Logging::GetInstance().SetUnit("FUSE@eosxd");
    eos::common::Logging::GetInstance().gShortFormat = true;
    eos::common::Logging::GetInstance().SetFilter("DumpStatistic");

    if (config.options.debug) {
      eos::common::Logging::GetInstance().SetLogPriority(LOG_DEBUG);
    } else {
      if (config.options.debuglevel) {
	eos::common::Logging::GetInstance().SetLogPriority(config.options.debuglevel);
      } else {
	eos::common::Logging::GetInstance().SetLogPriority(LOG_INFO);
      }
    }

#else
    eos::common::Logging::Init();
    eos::common::Logging::SetUnit("FUSE@eosxd");
    eos::common::Logging::gShortFormat = true;
    eos::common::Logging::SetFilter("DumpStatistic");

    if (config.options.debug) {
      eos::common::Logging::SetLogPriority(LOG_DEBUG);
    } else {
      if (config.options.debuglevel) {
	eos::common::Logging::SetLogPriority(config.options.debuglevel);
      } else {
	eos::common::Logging::SetLogPriority(LOG_INFO);
      }
    }

#endif
    // initialize mKV in case no cache is configured to act as no-op
    mKV.reset(new RedisKV());
#ifdef ROCKSDB_FOUND

    if (!config.mdcachedir.empty()) {
      RocksKV* kv = new RocksKV();

      if (kv->connect(config.name, config.mdcachedir) != 0) {
	fprintf(stderr, "error: failed to open rocksdb KV cache - path=%s",
		config.mdcachedir.c_str());
	exit(EINVAL);
      }

      mKV.reset(kv);
    }

#endif

    if (config.mdcachehost.length()) {
      RedisKV* kv = new RedisKV();

      if (kv->connect(config.name, config.mdcachehost, config.mdcacheport ?
		      config.mdcacheport : 6379) != 0) {
	fprintf(stderr, "error: failed to connect to md cache - connect-string=%s",
		config.mdcachehost.c_str());
	exit(EINVAL);
      }

      mKV.reset(kv);
    }

    mdbackend.init(config.hostport, config.remotemountdir,
		   config.options.md_backend_timeout,
		   config.options.md_backend_put_timeout);
    mds.init(&mdbackend);
    caps.init(&mdbackend, &mds);
    datas.init();

    if (config.mqtargethost.length()) {
      if (mds.connect(config.mqtargethost, config.mqidentity, config.mqname,
		      config.clienthost, config.clientuuid)) {
	fprintf(stderr,
		"error: failed to connect to mgm/zmq - connect-string=%s connect-identity=%s connect-name=%s",
		config.mqtargethost.c_str(), config.mqidentity.c_str(), config.mqname.c_str());
	exit(EINVAL);
      }
    }

    if (cachehandler::instance().init_daemonized()) {
      exit(errno);
    }

    fusestat.Add("getattr", 0, 0, 0);
    fusestat.Add("setattr", 0, 0, 0);
    fusestat.Add("setattr:chown", 0, 0, 0);
    fusestat.Add("setattr:chmod", 0, 0, 0);
    fusestat.Add("setattr:utimes", 0, 0, 0);
    fusestat.Add("setattr:truncate", 0, 0, 0);
    fusestat.Add("lookup", 0, 0, 0);
    fusestat.Add("opendir", 0, 0, 0);
    fusestat.Add("readdir", 0, 0, 0);
    fusestat.Add("releasedir", 0, 0, 0);
    fusestat.Add("statfs", 0, 0, 0);
    fusestat.Add("mknod", 0, 0, 0);
    fusestat.Add("mkdir", 0, 0, 0);
    fusestat.Add("rm", 0, 0, 0);
    fusestat.Add("unlink", 0, 0, 0);
    fusestat.Add("rmdir", 0, 0, 0);
    fusestat.Add("rename", 0, 0, 0);
    fusestat.Add("access", 0, 0, 0);
    fusestat.Add("open", 0, 0, 0);
    fusestat.Add("create", 0, 0, 0);
    fusestat.Add("read", 0, 0, 0);
    fusestat.Add("write", 0, 0, 0);
    fusestat.Add("release", 0, 0, 0);
    fusestat.Add("fsync", 0, 0, 0);
    fusestat.Add("forget", 0, 0, 0);
    fusestat.Add("flush", 0, 0, 0);
    fusestat.Add("getxattr", 0, 0, 0);
    fusestat.Add("setxattr", 0, 0, 0);
    fusestat.Add("listxattr", 0, 0, 0);
    fusestat.Add("removexattr", 0, 0, 0);
    fusestat.Add("readlink", 0, 0, 0);
    fusestat.Add("symlink", 0, 0, 0);
    fusestat.Add("link", 0, 0, 0);
    fusestat.Add(__SUM__TOTAL__, 0, 0, 0);
    tDumpStatistic.reset(&EosFuse::DumpStatistic, this);
    tStatCirculate.reset(&EosFuse::StatCirculate, this);
    tMetaCacheFlush.reset(&metad::mdcflush, &mds);
    tMetaCommunicate.reset(&metad::mdcommunicate, &mds);
    tCapFlush.reset(&cap::capflush, &caps);
    eos_static_warning("********************************************************************************");
    eos_static_warning("eosxd started version %s - FUSE protocol version %d",
		       VERSION, FUSE_USE_VERSION);
    eos_static_warning("eos-instance-url       := %s", config.hostport.c_str());
    eos_static_warning("thread-pool            := %s",
		       config.options.libfusethreads ? "libfuse" : "custom");
    eos_static_warning("zmq-connection         := %s", config.mqtargethost.c_str());
    eos_static_warning("zmq-identity           := %s", config.mqidentity.c_str());
    eos_static_warning("fd-limit               := %lu", config.options.fdlimit);
    eos_static_warning("options                := backtrace=%d md-cache:%d md-enoent:%.02f md-timeout:%.02f md-put-timeout:%.02f data-cache:%d mkdir-sync:%d create-sync:%d symlink-sync:%d rename-sync:%d rmdir-sync:%d flush:%d flush-w-open:%d locking:%d no-fsync:%s ol-mode:%03o show-tree-size:%d free-md-asap:%d core-affinity:%d no-xattr:%d no-link:%d nocache-graceperiod:%d rm-rf-protect-level=%d rm-rf-bulk=%d",
		       config.options.enable_backtrace,
		       config.options.md_kernelcache,
		       config.options.md_kernelcache_enoent_timeout,
		       config.options.md_backend_timeout,
		       config.options.md_backend_put_timeout,
		       config.options.data_kernelcache,
		       config.options.mkdir_is_sync,
		       config.options.create_is_sync,
		       config.options.symlink_is_sync,
		       config.options.rename_is_sync,
		       config.options.rmdir_is_sync,
		       config.options.global_flush,
		       config.options.flush_wait_open,
		       config.options.global_locking,
		       no_fsync_list.c_str(),
		       config.options.overlay_mode,
		       config.options.show_tree_size,
		       config.options.free_md_asap,
		       config.options.cpu_core_affinity,
		       config.options.no_xattr,
		       config.options.no_hardlinks,
		       config.options.nocache_graceperiod,
		       config.options.rm_rf_protect_levels,
		       config.options.rm_rf_bulk
		      );
    eos_static_warning("cache                  := rh-type:%s rh-nom:%d rh-max:%d rh-blocks:%d tot-size=%ld tot-ino=%ld dc-loc:%s jc-loc:%s clean-thrs:%02f%%%",
		       cconfig.read_ahead_strategy.c_str(),
		       cconfig.default_read_ahead_size,
		       cconfig.max_read_ahead_size,
		       cconfig.max_read_ahead_blocks,
		       cconfig.total_file_cache_size,
		       cconfig.total_file_cache_inodes,
		       cconfig.location.c_str(),
		       cconfig.journal.c_str(),
		       cconfig.clean_threshold);
    eos_static_warning("read-recovery          := enabled:%d ropen:%d ropen-noserv:%d ropen-noserv-window:%u",
		       config.recovery.read,
		       config.recovery.read_open,
		       config.recovery.read_open_noserver,
		       config.recovery.read_open_noserver_retrywindow);
    eos_static_warning("write-recovery         := enabled:%d wopen:%d wopen-noserv:%d wopen-noserv-window:%u",
		       config.recovery.write,
		       config.recovery.write_open,
		       config.recovery.write_open_noserver,
		       config.recovery.write_open_noserver_retrywindow);
    eos_static_warning("file-inlining          := emabled:%d max-size=%lu compressor=%s",
		       config.inliner.max_size ? 1 : 0,
		       config.inliner.max_size,
		       config.inliner.default_compressor.c_str());
    std::string xrdcl_option_string;
    std::string xrdcl_option_loglevel;

    for (auto it = xrdcl_options.begin(); it != xrdcl_options.end(); ++it) {
      xrdcl_option_string += *it;
      xrdcl_option_string += ":";
      int value = 0;
      std::string svalue;
      XrdCl::DefaultEnv::GetEnv()->GetInt(it->c_str(), value);
      xrdcl_option_string += eos::common::StringConversion::GetSizeString(svalue,
			     (unsigned long long) value);
      xrdcl_option_string += " ";
    }

    XrdCl::DefaultEnv::GetEnv()->GetString("LogLevel", xrdcl_option_loglevel);
    eos_static_warning("xrdcl-options          := %s log-level='%s' fusex-chunk-timeout=%d",
		       xrdcl_option_string.c_str(), xrdcl_option_loglevel.c_str(),
		       XrdCl::Proxy::sChunkTimeout);
    fusesession = fuse_lowlevel_new(&args,
				    &(get_operations()),
				    sizeof(operations), NULL);

    if ((fusesession != NULL)) {
      if (fuse_set_signal_handlers(fusesession) != -1) {
	fuse_session_add_chan(fusesession, fusechan);

	if (getenv("EOS_FUSE_NO_MT") &&
	    (!strcmp(getenv("EOS_FUSE_NO_MT"), "1"))) {
	  err = fuse_session_loop(fusesession);
	} else {
#if ( FUSE_USE_VERSION <= 28 )
	  err = fuse_session_loop_mt(fusesession);
#else

	  if (config.options.libfusethreads) {
	    err = fuse_session_loop_mt(fusesession);
	  } else {
	    EosFuseSessionLoop loop(10, 20, 10, 20);
	    err = loop.Loop(fusesession);
	  }

#endif
	}
      }
    }

    eos_static_warning("eosxd stopped version %s - FUSE protocol version %d",
		       VERSION, FUSE_USE_VERSION);
    eos_static_warning("********************************************************************************");
    tDumpStatistic.join();
    tStatCirculate.join();
    tMetaCacheFlush.join();
    tMetaCommunicate.join();
    tCapFlush.join();
    Mounter().terminate();

    // remove the session and channel object after all threads are joined
    if (fusesession) {
      fuse_remove_signal_handlers(fusesession);

      if (fusechan) {
	fuse_session_remove_chan(fusechan);
      }

      fuse_session_destroy(fusesession);
    }

    fuse_unmount(local_mount_dir, fusechan);
    mKV.reset();
  } else {
    fprintf(stderr, "error: failed to daemonize\n");
    exit(errno ? errno : -1);
  }

  return err ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::umounthandler(int sig, siginfo_t* si, void* ctx)
/* -------------------------------------------------------------------------- */
{
  eos::common::handleSignal(sig, si, ctx);
  std::string systemline = "fusermount -u -z ";
  systemline += EosFuse::Instance().Config().localmountdir;
  system(systemline.c_str());
  eos_static_warning("executing %s", systemline.c_str());
  eos_static_warning("sighandler received signal %d - emitting signal %d again",
		     sig, sig);
  signal(SIGSEGV, SIG_DFL);
  signal(SIGABRT, SIG_DFL);
  kill(getpid(), sig);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::init(void* userdata, struct fuse_conn_info* conn)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");

  if (EosFuse::instance().config.options.enable_backtrace) {
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = EosFuse::umounthandler;

    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
      char msg[1024];
      snprintf(msg, sizeof(msg), "failed to install SEGV handler");
      throw std::runtime_error(msg);
    }

    if (sigaction(SIGABRT, &sa, NULL) == -1) {
      char msg[1024];
      snprintf(msg, sizeof(msg), "failed to install SEGV handler");
      throw std::runtime_error(msg);
    }
  }

  conn->want |= FUSE_CAP_EXPORT_SUPPORT | FUSE_CAP_POSIX_LOCKS |
		FUSE_CAP_BIG_WRITES;
  conn->capable |= FUSE_CAP_EXPORT_SUPPORT | FUSE_CAP_POSIX_LOCKS |
		   FUSE_CAP_BIG_WRITES;
}

void
EosFuse::destroy(void* userdata)
{
  eos_static_debug("");
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::DumpStatistic(ThreadAssistant& assistant)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("started statistic dump thread");
  char ino_stat[16384];
  time_t start_time = time(NULL);

  while (!assistant.terminationRequested()) {
    eos::common::LinuxStat::linux_stat_t osstat;
#ifndef __APPLE__
    eos::common::LinuxMemConsumption::linux_mem_t mem;

    if (!eos::common::LinuxMemConsumption::GetMemoryFootprint(mem)) {
      eos_static_err("failed to get the MEM usage information");
    }

    if (!eos::common::LinuxStat::GetStat(osstat)) {
      eos_static_err("failed to get the OS usage information");
    }

#endif
    eos_static_debug("dumping statistics");
    XrdOucString out;
    fusestat.PrintOutTotal(out);
    std::string sout = out.c_str();
    time_t now = time(NULL);
    snprintf(ino_stat, sizeof(ino_stat),
	     "# -----------------------------------------------------------------------------------------------------------\n"
	     "ALL        inodes              := %lu\n"
	     "ALL        inodes stack        := %lu\n"
	     "ALL        inodes-todelete     := %lu\n"
	     "ALL        inodes-backlog      := %lu\n"
	     "ALL        inodes-ever         := %lu\n"
	     "ALL        inodes-ever-deleted := %lu\n"
	     "ALL        inodes-open         := %lu\n"
	     "ALL        inodes-vmap         := %lu\n"
	     "ALL        inodes-caps         := %lu\n"
	     "# -----------------------------------------------------------------------------------------------------------\n",
	     this->getMdStat().inodes(),
	     this->getMdStat().inodes_stacked(),
	     this->getMdStat().inodes_deleted(),
	     this->getMdStat().inodes_backlog(),
	     this->getMdStat().inodes_ever(),
	     this->getMdStat().inodes_deleted_ever(),
	     this->datas.size(),
	     this->mds.vmaps().size(),
	     this->caps.size()
	    );
    sout += ino_stat;
    std::string s1;
    std::string s2;
    std::string s3;
    std::string s4;
    std::string s5;
    std::string s6;
    std::string s7;
    std::string s8;
    snprintf(ino_stat, sizeof(ino_stat),
	     "ALL        threads             := %llu\n"
	     "ALL        visze               := %s\n"
	     "All        rss                 := %s\n"
	     "All        wr-buf-inflight     := %s\n"
	     "All        wr-buf-queued       := %s\n"
	     "All        ra-buf-inflight     := %s\n"
	     "All        ra-buf-queued       := %s\n"
	     "All        rd-buf-inflight     := %s\n"
	     "All        rd-buf-queued       := %s\n"
	     "All        version             := %s\n"
	     "ALl        fuseversion         := %d\n"
	     "All        starttime           := %lu\n"
	     "All        uptime              := %lu\n"
	     "All        instance-url        := %s\n"
	     "All        client-uuid         := %s\n"
	     "# -----------------------------------------------------------------------------------------------------------\n",
	     osstat.threads,
	     eos::common::StringConversion::GetReadableSizeString(s1, osstat.vsize, "b"),
	     eos::common::StringConversion::GetReadableSizeString(s2, osstat.rss, "b"),
	     eos::common::StringConversion::GetReadableSizeString(s3,
		 XrdCl::Proxy::sWrBufferManager.inflight(), "b"),
	     eos::common::StringConversion::GetReadableSizeString(s4,
		 XrdCl::Proxy::sWrBufferManager.queued(), "b"),
	     eos::common::StringConversion::GetReadableSizeString(s5,
		 XrdCl::Proxy::sRaBufferManager.inflight(), "b"),
	     eos::common::StringConversion::GetReadableSizeString(s6,
		 XrdCl::Proxy::sRaBufferManager.queued(), "b"),
	     eos::common::StringConversion::GetReadableSizeString(s7,
		 data::datax::sBufferManager.inflight(), "b"),
	     eos::common::StringConversion::GetReadableSizeString(s8,
		 data::datax::sBufferManager.queued(), "b"),
	     VERSION,
	     FUSE_USE_VERSION,
	     start_time,
	     now - start_time,
	     EosFuse::Instance().config.hostport.c_str(),
	     EosFuse::instance().config.clientuuid.c_str()
	    );
    sout += ino_stat;
    std::ofstream dumpfile(EosFuse::Instance().config.statfilepath);
    dumpfile << sout;
    assistant.wait_for(std::chrono::seconds(1));
  }
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::StatCirculate(ThreadAssistant& assistant)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("started stat circulate thread");
  fusestat.Circulate(assistant);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  struct fuse_entry_param e;
  metad::shared_md md = Instance().mds.getlocal(req, ino);
  {
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || (md->deleted() && !md->lookup_is())) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      fuse_ino_t cap_ino = S_ISDIR(md->mode()) ? ino : md->pid();
      cap::shared_cap pcap = Instance().caps.acquire(req, cap_ino ? cap_ino : 1,
			     S_IFDIR | X_OK | R_OK);
      XrdSysMutexHelper capLock(pcap->Locker());

      if (pcap->errc()) {
	rc = pcap->errc();
      } else {
	md->convert(e);
	eos_static_info("%s", md->dump(e).c_str());
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_attr(req, &e.attr, e.attr_timeout);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, fi, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int op,
		 struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  cap::shared_cap pcap;
  metad::shared_md md;
  md = Instance().mds.get(req, ino);
  md->Locker().Lock();

  if (op == 0) {
    rc = EINVAL;
  } else if (!md->id() || (md->deleted() && !md->lookup_is())) {
    rc = md->deleted() ? ENOENT : md->err();
  } else {
    fuse_ino_t cap_ino = S_ISDIR(md->mode()) ? ino : md->pid();

    if (op & FUSE_SET_ATTR_MODE) {
      // chmod permissions are derived from the parent in case of a directory or file
      // otherwise we trap ourselfs when revoking W_OK
      if (S_ISDIR(md->mode())) {
	cap_ino = md->pid();
      }

      // retrieve cap for mode setting
      pcap = Instance().caps.acquire(req, cap_ino,
				     M_OK);
    } else if ((op & FUSE_SET_ATTR_UID) || (op & FUSE_SET_ATTR_GID)) {
      // retrieve cap for owner setting
      pcap = Instance().caps.acquire(req, cap_ino,
				     C_OK);
    } else if (op & FUSE_SET_ATTR_SIZE) {
      // retrieve cap for write
      pcap = Instance().caps.acquire(req, cap_ino,
				     W_OK);
    } else if ((op & FUSE_SET_ATTR_ATIME)
	       || (op & FUSE_SET_ATTR_MTIME)
	       || (op & FUSE_SET_ATTR_ATIME_NOW)
	       || (op & FUSE_SET_ATTR_MTIME_NOW)
	      ) {
      // retrieve cap for write
      pcap = Instance().caps.acquire(req, cap_ino,
				     W_OK);

      if (pcap->errc()) {
	// retrieve cap for set utime
	pcap = Instance().caps.acquire(req, cap_ino,
				       SU_OK);
      }
    }

    if (pcap->errc()) {
      rc = pcap->errc();
    } else {
      if (op & FUSE_SET_ATTR_MODE) {
	/*
	  EACCES Search permission is denied on a component of the path prefix.

	  EFAULT path points outside your accessible address space.

	  EIO    An I/O error occurred.

	  ELOOP  Too many symbolic links were encountered in resolving path.

	  ENAMETOOLONG
		 path is too long.

	  ENOENT The file does not exist.

	  ENOMEM Insufficient kernel memory was available.

	  ENOTDIR
		 A component of the path prefix is not a directory.

	  EPERM  The  effective  UID does not match the owner of the file,
		 and the process is not privileged (Linux: it does not

		 have the CAP_FOWNER capability).

	  EROFS  The named file resides on a read-only filesystem.

	  The general errors for fchmod() are listed below:

	  EBADF  The file descriptor fd is not valid.

	  EIO    See above.

	  EPERM  See above.

	  EROFS  See above.
	 */
	ADD_FUSE_STAT("setattr:chmod", req);
	EXEC_TIMING_BEGIN("setattr:chmod");
	md->set_mode(attr->st_mode);
	EXEC_TIMING_END("setattr:chmod");
      }

      if ((op & FUSE_SET_ATTR_UID) || (op & FUSE_SET_ATTR_GID)) {
	/*
	  EACCES Search permission is denied on a component of the path prefix.

	  EFAULT path points outside your accessible address space.

	  ELOOP  Too many symbolic links were encountered in resolving path.

	  ENAMETOOLONG
		 path is too long.

	  ENOENT The file does not exist.

	  ENOMEM Insufficient kernel memory was available.

	  ENOTDIR
		 A component of the path prefix is not a directory.

	  EPERM  The calling process did not have the required permissions
		 (see above) to change owner and/or group.

	  EROFS  The named file resides on a read-only filesystem.

	  The general errors for fchown() are listed below:

	  EBADF  The descriptor is not valid.

	  EIO    A low-level I/O error occurred while modifying the inode.

	  ENOENT See above.

	  EPERM  See above.

	  EROFS  See above.
	 */
	ADD_FUSE_STAT("setattr:chown", req);
	EXEC_TIMING_BEGIN("setattr:chown");

	if (op & FUSE_SET_ATTR_UID) {
	  md->set_uid(attr->st_uid);
	}

	if (op & FUSE_SET_ATTR_GID) {
	  md->set_gid(attr->st_gid);
	}

	EXEC_TIMING_END("setattr:chown");
      }

      if (
	(op & FUSE_SET_ATTR_ATIME)
	|| (op & FUSE_SET_ATTR_MTIME)
	|| (op & FUSE_SET_ATTR_ATIME_NOW)
	|| (op & FUSE_SET_ATTR_MTIME_NOW)
      ) {
	/*
	EACCES Search permission is denied for one of the directories in
	the  path  prefix  of  path

	EACCES times  is  NULL,  the caller's effective user ID does not match
	the owner of the file, the caller does not have
	write access to the file, and the caller is not privileged
	(Linux: does not have either the CAP_DAC_OVERRIDE or
	the CAP_FOWNER capability).

	ENOENT filename does not exist.

	EPERM  times is not NULL, the caller's effective UID does not
	match the owner of the file, and the caller is not priv‐
	ileged (Linux: does not have the CAP_FOWNER capability).

	EROFS  path resides on a read-only filesystem.
	 */
	ADD_FUSE_STAT("setattr:utimes", req);
	EXEC_TIMING_BEGIN("setattr:utimes");
	struct timespec tsnow;
	eos::common::Timing::GetTimeSpec(tsnow);

	if (op & FUSE_SET_ATTR_ATIME) {
	  md->set_atime(attr->ATIMESPEC.tv_sec);
	  md->set_atime_ns(attr->ATIMESPEC.tv_nsec);
	  md->set_ctime(tsnow.tv_sec);
	  md->set_ctime_ns(tsnow.tv_nsec);
	}

	if (op & FUSE_SET_ATTR_MTIME) {
	  md->set_mtime(attr->MTIMESPEC.tv_sec);
	  md->set_mtime_ns(attr->MTIMESPEC.tv_nsec);
	  md->set_ctime(tsnow.tv_sec);
	  md->set_ctime_ns(tsnow.tv_nsec);
	}

	if ((op & FUSE_SET_ATTR_ATIME_NOW) ||
	    (op & FUSE_SET_ATTR_MTIME_NOW)) {
	  if (op & FUSE_SET_ATTR_ATIME_NOW) {
	    md->set_atime(tsnow.tv_sec);
	    md->set_atime_ns(tsnow.tv_nsec);
	    md->set_ctime(tsnow.tv_sec);
	    md->set_ctime_ns(tsnow.tv_nsec);
	  }

	  if (op & FUSE_SET_ATTR_MTIME_NOW) {
	    md->set_mtime(tsnow.tv_sec);
	    md->set_mtime_ns(tsnow.tv_nsec);
	    md->set_ctime(tsnow.tv_sec);
	    md->set_ctime_ns(tsnow.tv_nsec);
	  }
	}

	std::string cookie = md->Cookie();
	Instance().datas.update_cookie(md->id(), cookie);
	EXEC_TIMING_END("setattr:utimes");
      }

      if (op & FUSE_SET_ATTR_SIZE) {
	/*
	EACCES Search  permission is denied for a component of the path
	prefix, or the named file is not writable by the user.

	EFAULT Path points outside the process's allocated address space.

	EFBIG  The argument length is larger than the maximum file size.

	EINTR  While blocked waiting to complete, the call was interrupted
	by a signal handler; see fcntl(2) and signal(7).

	EINVAL The argument length is negative or larger than the maximum
	file size.

	EIO    An I/O error occurred updating the inode.

	EISDIR The named file is a directory.

	ELOOP  Too many symbolic links were encountered in translating the
	pathname.

	ENAMETOOLONG
	A component of a pathname exceeded 255 characters, or an
	entire pathname exceeded 1023 characters.

	ENOENT The named file does not exist.

	ENOTDIR
	A component of the path prefix is not a directory.

	EPERM  The underlying filesystem does not support extending a file
	beyond its current size.

	EROFS  The named file resides on a read-only filesystem.

	ETXTBSY
	The file is a pure procedure (shared text) file that is
	being executed.

	For ftruncate() the same errors apply, but instead of things that
	can be wrong with path, we now have things that  can
	be wrong with the file descriptor, fd:

	EBADF  fd is not a valid descriptor.

	EBADF or EINVAL
	fd is not open for writing.

	EINVAL fd does not reference a regular file.
	 */
	ADD_FUSE_STAT("setattr:truncate", req);
	EXEC_TIMING_BEGIN("setattr:truncate");
	int rc = 0;

	if (!md->id() || (md->deleted() && !md->lookup_is())) {
	  rc = ENOENT;
	} else {
	  if ((md->mode() & S_IFDIR)) {
	    rc = EISDIR;
	  } else {
	    if (fi && fi->fh) {
	      // ftruncate
	      data::data_fh* io = (data::data_fh*) fi->fh;

	      if (io) {
		eos_static_debug("ftruncate size=%lu", (size_t) attr->st_size);
		rc |= io->ioctx()->truncate(req, attr->st_size);
		io->ioctx()->inline_file(attr->st_size);
		rc |= io->ioctx()->flush(req);
		rc = rc ? (errno ? errno : rc) : 0;
	      } else {
		rc = EIO;
	      }
	    } else {
	      // truncate
	      eos_static_debug("truncate size=%lu", (size_t) attr->st_size);
	      std::string cookie = md->Cookie();
	      data::shared_data io = Instance().datas.get(req, md->id(), md);
	      rc = io->attach(req, cookie, true);
	      eos_static_debug("calling truncate");
	      rc |= io->truncate(req, attr->st_size);
	      io->inline_file(attr->st_size);
	      rc |= io->flush(req);
	      rc |= io->detach(req, cookie, true);
	      rc = rc ? (errno ? errno : rc) : 0;
	      Instance().datas.release(req, md->id());
	    }

	    if (!rc) {
	      ssize_t size_change = (int64_t)(attr->st_size) - (int64_t) md->size();

	      if (size_change > 0) {
		Instance().caps.book_volume(pcap, size_change);
	      } else {
		Instance().caps.free_volume(pcap, size_change);
	      }

	      md->set_size(attr->st_size);
	    }
	  }
	}

	EXEC_TIMING_END("setattr:truncate");
      }
    }
  }

  if (rc) {
    md->Locker().UnLock();
    fuse_reply_err(req, rc);
  } else {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    md->convert(e);
    eos_static_info("%s", md->dump(e).c_str());
    Instance().mds.update(req, md, pcap->authid());
    md->Locker().UnLock();
    fuse_reply_attr(req, &e.attr, e.attr_timeout);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, fi, rc).c_str());
}

void
EosFuse::lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  struct fuse_entry_param e;
  memset(&e, 0, sizeof(e));
  {
    metad::shared_md md;
    md = Instance().mds.lookup(req, parent, name);

    if (md->id() && !md->deleted()) {
      XrdSysMutexHelper mLock(md->Locker());
      md->set_pid(parent);
      md->convert(e);
      eos_static_info("%s", md->dump(e).c_str());
      md->lookup_inc();
      cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			     R_OK);
    } else {
      // negative cache entry
      e.ino = 0;
      e.attr_timeout = Instance().Config().options.md_kernelcache_enoent_timeout;
      e.entry_timeout = Instance().Config().options.md_kernelcache_enoent_timeout;

      if (e.entry_timeout) {
	rc = 0;
      } else {
	rc = md->deleted() ? ENOENT : md->err();
      }
    }

    if (md->err()) {
      if (EOS_LOGS_DEBUG) {
	eos_static_debug("returning errc=%d for ino=%#lx name=%s md-name=%s\n",
			 md->err(), parent, name, md->name().c_str());
      }

      rc = md->err();
    }
  }
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f name=%s %s", timing.RealTime(), name,
		    dump(id, parent, 0, rc).c_str());

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_entry(req, &e);
  }
}

/* -------------------------------------------------------------------------- */
int
/* -------------------------------------------------------------------------- */
EosFuse::listdir(fuse_req_t req, fuse_ino_t ino, metad::shared_md& md)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");
  int rc = 0;
  fuse_id id(req);
  // retrieve cap
  cap::shared_cap pcap = Instance().caps.acquire(req, ino,
			 S_IFDIR | X_OK | R_OK, true);
  XrdSysMutexHelper cLock(pcap->Locker());

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    // retrieve md
    std::string authid = pcap->authid();
    cLock.UnLock();
    md = Instance().mds.get(req, ino, authid, true);
  }

  return rc;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  EXEC_TIMING_BEGIN(__func__);
  ADD_FUSE_STAT(__func__, req);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  metad::shared_md md;

  if (isRecursiveRm(req, true, true) &&
      Instance().Config().options.rm_rf_bulk) {
    md = Instance().mds.get(req, ino);

    if (md && md->attr().count("sys.recycle")) {
      eos_static_warning("Running recursive rm (pid = %d)", fuse_req_ctx(req)->pid);
      // bulk rm only when a recycle bin is configured
      {
	XrdSysMutexHelper mLock(md->Locker());

	if (!md->id() || md->deleted()) {
	  rc = md->deleted() ? ENOENT : md->err();
	} else {
	  rc = Instance().mds.rmrf(req, md);
	}
      }

      if (!rc) {
	// invalide this directory
	Instance().mds.cleanup(md);
	metad::shared_md pmd = Instance().mds.getlocal(req, md->pid());

	if (pmd) {
	  pmd->local_children().erase(md->name());
	  pmd->mutable_children()->erase(md->name());
	}
      }
    }
  }

  rc = listdir(req, ino, md);

  if (!rc) {
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      eos_static_info("%s", md->dump().c_str());

      if (isRecursiveRm(req) &&
	  Instance().mds.calculateDepth(md) <=
	  Instance().Config().options.rm_rf_protect_levels) {
	eos_static_warning("Blocking recursive rm (pid = %d)", fuse_req_ctx(req)->pid);
	rc = EPERM; // you shall not pass, muahahahahah
      } else {
	auto md_fh = new opendir_t;
	md_fh->md = md;
	md->opendir_inc();
	// fh contains a dummy 0 pointer
	eos_static_debug("adding ino=%08lx p-ino=%08lx", md->id(), md->pid());
	fi->fh = (unsigned long) md_fh;
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_open(req, fi);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		 struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
/*
EBADF  Invalid directory stream descriptor fi->fh
 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  EXEC_TIMING_BEGIN(__func__);
  ADD_FUSE_STAT(__func__, req);
  int rc = 0;
  fuse_id id(req);

  if (!fi->fh) {
    fuse_reply_err(req, EBADF);
    rc = EBADF;
  } else {
    // get the shared pointer from the open file descriptor
    opendir_t* md = (opendir_t*) fi->fh;
    metad::shared_md pmd = md->md;
    std::map<std::string, uint64_t> pmd_children;
    mode_t pmd_mode;
    uint64_t pmd_id;
    {
      // avoid to have more than one md object locked at a time
      XrdSysMutexHelper mLock(pmd->Locker());

      // make sure, the meta-data object contains listing information (it might have been invalidated by a callback)
      do {
	if (pmd->type() == pmd->MDLS) {
	  break;
	}

	pmd->Locker().UnLock();
	// refresh the listing
	eos_static_debug("refresh listing int=%16lx", ino);
	rc = listdir(req, ino, pmd);
	pmd->Locker().Lock();
      } while ((!rc) && (pmd->type() != pmd->MDLS));

      pmd_mode = pmd->mode();
      pmd_id = pmd->id();
      auto pmap = pmd->local_children();
      auto it = pmap.begin();

      for (; it != pmap.end(); ++it) {
	pmd_children[it->first] = it->second;
      }

      if (!pmd_children.size()) {
	if (EOS_LOGS_DEBUG) {
	  eos_static_debug("%s", Instance().mds.dump_md(pmd, false).c_str());
	}
      }
    }
    // only one readdir at a time
    XrdSysMutexHelper lLock(md->items_lock);
    auto it = pmd_children.begin();
    eos_static_info("off=%lu size-%lu", off, pmd_children.size());
    char b[size];
    char* b_ptr = b;
    off_t b_size = 0;

    // the root directory adds only '.', all other add '.' and '..' for off=0
    if (off == 0) {
      // at offset=0 add the '.' directory
      std::string bname = ".";
      fuse_ino_t cino = pmd_id;
      eos_static_debug("list: %08x %s", cino, bname.c_str());
      mode_t mode = pmd_mode;
      struct stat stbuf;
      memset(&stbuf, 0, sizeof(struct stat));
      stbuf.st_ino = cino;
      stbuf.st_mode = mode;
      size_t a_size = fuse_add_direntry(req, b_ptr, size - b_size,
					bname.c_str(), &stbuf, ++off);
      eos_static_info("name=%s ino=%08lx mode=%08x bytes=%u/%u",
		      bname.c_str(), cino, mode, a_size, size - b_size);
      b_ptr += a_size;
      b_size += a_size;
      // at offset=0 add the '..' directory
      metad::shared_md ppmd = Instance().mds.get(req, pmd->pid(), "", true, 0, 0,
			      true);

      // don't add a '..' at root
      if ((cino > 1) && ppmd && (ppmd->id() == pmd->pid())) {
	fuse_ino_t cino = 0;
	mode_t mode = 0;
	{
	  XrdSysMutexHelper ppLock(ppmd->Locker());
	  cino = ppmd->id();
	  mode = ppmd->mode();
	}
	std::string bname = "..";
	eos_static_debug("list: %08x %s", cino, bname.c_str());
	struct stat stbuf;
	memset(&stbuf, 0, sizeof(struct stat));
	stbuf.st_ino = cino;
	stbuf.st_mode = mode;
	size_t a_size = fuse_add_direntry(req, b_ptr, size - b_size,
					  bname.c_str(), &stbuf, ++off);
	eos_static_info("name=%s ino=%08lx mode=%08x bytes=%u/%u",
			bname.c_str(), cino, mode, a_size, size - b_size);
	b_ptr += a_size;
	b_size += a_size;
      }
    }

    off_t i_offset = 2;

    // add regular children
    for (; it != pmd_children.end(); ++it) {
      if (off > i_offset) {
	i_offset++;
	continue;
      } else {
	i_offset++;
      }

      // skip entries we have shown already
      if (md->readdir_items.count(it->first)) {
	continue;
      }

      std::string bname = it->first;
      fuse_ino_t cino = it->second;
      metad::shared_md cmd = Instance().mds.get(req, cino, "", 0, 0, 0, true);
      eos_static_debug("list: %08x %s (d=%d)", cino, it->first.c_str(),
		       cmd->deleted());

      if (strncmp(bname.c_str(), "...eos.ino...",
		  13) == 0) { /* hard link deleted inodes */
	continue;
      }

      mode_t mode;
      {
	XrdSysMutexHelper cLock(cmd->Locker());
	mode = cmd->mode();

	// skip deleted entries or hidden entries
	if (cmd->deleted()) {
	  continue;
	}
      }
      struct stat stbuf;
      memset(&stbuf, 0, sizeof(struct stat));
      stbuf.st_ino = cino;
      {
	auto attrMap = cmd->attr();

	if (attrMap.count(k_mdino)) {
	  uint64_t mdino = std::stoll(attrMap[k_mdino]);
	  uint64_t local_ino = Instance().mds.vmaps().forward(mdino);

	  if (EOS_LOGS_DEBUG) {
	    eos_static_debug("hlnk %s id %#lx mdino '%s' (%lx) local_ino %#lx",
			     cmd->name().c_str(), cmd->id(), attrMap[k_mdino].c_str(), mdino, local_ino);
	  }

	  stbuf.st_ino = local_ino;
	  metad::shared_md target = Instance().mds.get(req, local_ino, "", 0, 0, 0,
				    true);
	  mode = target->mode();
	}
      }
      stbuf.st_mode = mode;
      size_t a_size = fuse_add_direntry(req, b_ptr, size - b_size,
					bname.c_str(), &stbuf, ++off);
      eos_static_info("name=%s id=%#lx ino=%#lx mode=%#o bytes=%u/%u",
		      bname.c_str(), cino, stbuf.st_ino, mode, a_size, size - b_size);

      if (a_size > (size - b_size)) {
	break;
      }

      // add to the shown list
      md->readdir_items.insert(it->first);
      b_ptr += a_size;
      b_size += a_size;
    }

    if (b_size) {
      fuse_reply_buf(req, b, b_size);
    } else {
      fuse_reply_buf(req, b, 0);
    }

    eos_static_debug("size=%lu off=%llu reply-size=%lu", size, off, b_size);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  EXEC_TIMING_BEGIN(__func__);
  ADD_FUSE_STAT(__func__, req);
  int rc = 0;
  fuse_id id(req);
  opendir_t* md = (opendir_t*) fi->fh;

  if (md) {
    // The following two lines act as a barrier to ensure the last readdir() has
    // released items_lock. From the point of view of the FUSE kernel module,
    // once we call fuse_reply_buf inside readdir, that syscall is over, and it
    // is free to call releasedir. This creates a race condition where we try to
    // delete md while readdir still holds items_lock - the following two lines
    // prevent this.
    md->items_lock.Lock();
    md->items_lock.UnLock();
    md->md->opendir_dec(1);
    delete md;
    fi->fh = 0;
  }

  EXEC_TIMING_END(__func__);
  fuse_reply_err(req, 0);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::statfs(fuse_req_t req, fuse_ino_t ino)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  struct statvfs svfs;
  memset(&svfs, 0, sizeof(struct statvfs));
  rc = Instance().mds.statvfs(req, &svfs);

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_statfs(req, &svfs);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode)
/* -------------------------------------------------------------------------- */
/*
EACCES The parent directory does not allow write permission to the process,
or one of the directories in pathname  did

not allow search permission.  (See also path_resolution(7).)

EDQUOT The user's quota of disk blocks or inodes on the filesystem has been
exhausted.

EEXIST pathname  already exists (not necessarily as a directory).
This includes the case where pathname is a symbolic
link, dangling or not.

EFAULT pathname points outside your accessible address space.

ELOOP  Too many symbolic links were encountered in resolving pathname.

EMLINK The number of links to the parent directory would exceed LINK_MAX.

ENAMETOOLONG
pathname was too long.

ENOENT A directory component in pathname does not exist or is a dangling
symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOSPC The device containing pathname has no room for the new directory.

ENOSPC The new directory cannot be created because the user's disk quota is
exhausted.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory.

EPERM  The filesystem containing pathname does not support the creation of
directories.

EROFS  pathname refers to a file on a read-only filesystem.
 */
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), parent, true);
  int rc = 0;
  fuse_id id(req);
  struct fuse_entry_param e;
  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | X_OK | W_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    metad::shared_md md;
    metad::shared_md pmd;
    uint64_t del_ino = 0;
    md = Instance().mds.lookup(req, parent, name);
    pmd = Instance().mds.get(req, parent, pcap->authid());
    {
      std::string implied_cid;
      {
	// logic avoiding a mkdir/rmdir/mkdir sync/async race
	{
	  XrdSysMutexHelper pLock(pmd->Locker());
	  auto it = pmd->get_todelete().find(name);

	  if ((it != pmd->get_todelete().end()) && it->second) {
	    del_ino = it->second;
	  }
	}

	if (del_ino) {
	  Instance().mds.wait_deleted(req, del_ino);
	}
      }
      XrdSysMutexHelper mLock(md->Locker());

      if (md->id() && !md->deleted()) {
	rc = EEXIST;
      } else {
	md->set_err(0);
	md->set_mode(mode | S_IFDIR);
	struct timespec ts;
	eos::common::Timing::GetTimeSpec(ts);
	md->set_name(name);
	md->set_atime(ts.tv_sec);
	md->set_atime_ns(ts.tv_nsec);
	md->set_mtime(ts.tv_sec);
	md->set_mtime_ns(ts.tv_nsec);
	md->set_ctime(ts.tv_sec);
	md->set_ctime_ns(ts.tv_nsec);
	md->set_btime(ts.tv_sec);
	md->set_btime_ns(ts.tv_nsec);
	// need to update the parent mtime
	md->set_pmtime(ts.tv_sec);
	md->set_pmtime_ns(ts.tv_nsec);
	pmd->set_mtime(ts.tv_sec);
	pmd->set_mtime_ns(ts.tv_nsec);
	md->set_uid(pcap->uid());
	md->set_gid(pcap->gid());
	md->set_id(Instance().mds.insert(req, md, pcap->authid()));
	md->set_nlink(2);
	md->set_creator(true);
	std::string imply_authid = eos::common::StringConversion::random_uuidstring();
	eos_static_info("generating implied authid %s => %s", pcap->authid().c_str(),
			imply_authid.c_str());
	implied_cid = Instance().caps.imply(pcap, imply_authid, mode,
					    (fuse_ino_t) md->id());
	md->cap_inc();
	md->set_implied_authid(imply_authid);
      }

      if (!rc) {
	if (Instance().Config().options.mkdir_is_sync) {
	  md->set_type(md->EXCL);
	  rc = Instance().mds.add_sync(req, pmd, md, pcap->authid());
	  md->set_type(md->MD);
	} else {
	  Instance().mds.add(req, pmd, md, pcap->authid());
	}

	if (!rc) {
	  memset(&e, 0, sizeof(e));
	  md->convert(e);
	  md->lookup_inc();
	  eos_static_info("%s", md->dump(e).c_str());
	} else {
	  Instance().getCap().forget(implied_cid);
	}
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_entry(req, &e);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc, name).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::unlink(fuse_req_t req, fuse_ino_t parent, const char* name)
/* -------------------------------------------------------------------------- */
/*
EACCES Write access to the directory containing pathname is not allowed for the process's effective UID, or one of the
directories in pathname did not allow search permission.  (See also path_resolution(7).)

EBUSY  The file pathname cannot be unlinked because it is being used by the system or another process; for example, it
is a mount point or the NFS client software created it to represent an  active  but  otherwise  nameless  inode
("NFS silly renamed").

EFAULT pathname points outside your accessible address space.

EIO    An I/O error occurred.

EISDIR pathname refers to a directory.  (This is the non-POSIX value returned by Linux since 2.1.132.)

ELOOP  Too many symbolic links were encountered in translating pathname.

ENAMETOOLONG
pathname was too long.

ENOENT A component in pathname does not exist or is a dangling symbolic link, or pathname is empty.

ENOMEM Insufficient kernel memory was available.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory.

EPERM  The  system  does  not allow unlinking of directories, or unlinking of directories requires privileges that the
calling process doesn't have.  (This is the POSIX prescribed error return; as noted above, Linux returns EISDIR
for this case.)

EPERM (Linux only)
The filesystem does not allow unlinking of files.

EPERM or EACCES
The  directory  containing pathname has the sticky bit (S_ISVTX) set and the process's effective UID is neither
the UID of the file to be deleted nor that of the directory containing it, and the process  is  not  privileged
(Linux: does not have the CAP_FOWNER capability).

EROFS  pathname refers to a file on a read-only filesystem.

 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  if (EOS_LOGS_DEBUG) {
    eos_static_debug("parent=%#lx name=%s", parent, name);
  }

  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  fuse_ino_t hardlink_target_ino = 0;
  Track::Monitor pmon(__func__, Instance().Tracker(), parent, true);
  int rc = 0;
  fuse_id id(req);
  // retrieve cap
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | X_OK | D_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    metad::shared_md pmd = NULL /* Parent */, tmd = NULL /*Hard link target */;
    std::string sname = name;
    uint64_t freesize = 0;

    if (sname == ".") {
      rc = EINVAL;
    }

    if (sname.length() > 1024) {
      rc = ENAMETOOLONG;
    }

    fuse_ino_t del_ino = 0;

    if (!rc) {
      metad::shared_md md;
      md = Instance().mds.lookup(req, parent, name);
      XrdSysMutexHelper mLock(md->Locker());

      if (!md->id() || md->deleted()) {
	rc = ENOENT;
      }

      if ((!rc) && ((md->mode() & S_IFDIR))) {
	rc = EISDIR;
      }

      if (!rc) {
	if (isRecursiveRm(req) &&
	    Instance().Config().options.rm_rf_protect_levels &&
	    Instance().mds.calculateDepth(md) <=
	    Instance().Config().options.rm_rf_protect_levels) {
	  eos_static_warning("Blocking recursive rm (pid = %d )", fuse_req_ctx(req)->pid);
	  rc = EPERM; // you shall not pass, muahahahahah
	} else {
	  del_ino = md->id();
	  int nlink =
	    0; /* nlink has 0-origin (0 = simple file, 1 = inode has two names) */
	  auto attrMap = md->attr();
	  pmd = Instance().mds.get(req, parent, pcap->authid());

	  if (attrMap.count(k_mdino)) { /* This is a hard link */
	    uint64_t mdino = std::stoll(attrMap[k_mdino]);
	    uint64_t local_ino = Instance().mds.vmaps().forward(mdino);
	    tmd = Instance().mds.get(req, local_ino,
				     pcap->authid()); /* the target of the link */
	    attrMap = tmd->attr();
	    hardlink_target_ino = tmd->id();
	  }

	  if (tmd) {
	    // update the counting on the target
	    auto attrMap = tmd->attr();

	    if (attrMap.count(k_nlink)) {
	      nlink = std::stol(attrMap[k_nlink]);

	      // do the counting locally
	      if (nlink > 0) {
		auto wAttrMap = tmd->mutable_attr();
		(*wAttrMap)[k_nlink] = std::to_string(nlink - 1);
		eos_static_debug("setting link count to %d-1", nlink);
	      }

	      tmd->set_nlink(nlink);
	    }
	  } else {
	    if (attrMap.count(k_nlink)) {
	      nlink = std::stol(attrMap[k_nlink]);

	      if (nlink != 0) {
		tmd = md;
	      }

	      if (nlink > 0) {
		auto wAttrMap = tmd->mutable_attr();
		(*wAttrMap)[k_nlink] = std::to_string(nlink - 1);
		eos_static_debug("setting link count to %d-1", nlink);
	      }

	      if (tmd) {
		tmd->set_nlink(nlink);
	      }
	    }
	  }

	  if (nlink <= 0) { /* don't bother updating, this is a real delete */
	    freesize = md->size();
	  }

	  if (EOS_LOGS_DEBUG) {
	    eos_static_debug("hlnk unlink %s new nlink %d %s", name, nlink,
			     Instance().mds.dump_md(md, false).c_str());
	  }

	  // we have to signal the unlink always to 'the' target inode of a hardlink
	  if (hardlink_target_ino) {
	    Instance().datas.unlink(req, hardlink_target_ino);
	  } else {
	    Instance().datas.unlink(req, md->id());
	  }

	  if (md != tmd) {
	    Instance().mds.remove(req, pmd, md, pcap->authid());

	    if (tmd && (tmd->nlink() == 0)) {
	      // delete the target locally
	      Instance().mds.remove(req, pmd, tmd, pcap->authid(), false);
	    }
	  } else {
	    // if a hardlink target is deleted, we create a shadow entry until all
	    // references are removed
	    char nameBuf[256];
	    snprintf(nameBuf, sizeof(nameBuf), "...eos.ino...%lx", md->md_ino());
	    std::string newname = nameBuf;
	    md->Locker().UnLock();
	    Instance().mds.mv(req, pmd, pmd, md, newname, pcap->authid(),
			      pcap->authid());
	    md->Locker().Lock();
	  }
	}
      }
    }

    if (!rc) {
      if (hardlink_target_ino || Instance().Config().options.rmdir_is_sync) {
	eos_static_debug("waiting for flush of  %d", del_ino);
	Instance().mds.wait_deleted(req, del_ino);
      }

      XrdSysMutexHelper pLock(pcap->Locker());
      Instance().caps.free_volume(pcap, freesize);
      Instance().caps.free_inode(pcap);
      eos_static_debug("freeing %llu bytes on cap ", freesize);
    }
  }

  fuse_reply_err(req, rc);

  // the link count has changed and we have to tell the kernel cache
  if (hardlink_target_ino &&
      EosFuse::Instance().Config().options.md_kernelcache) {
    eos_static_warning("invalidating inode %d", hardlink_target_ino);
    kernelcache::inval_inode(hardlink_target_ino, true);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc, name).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rmdir(fuse_req_t req, fuse_ino_t parent, const char* name)
/* -------------------------------------------------------------------------- */
/*
EACCES Write access to the directory containing pathname was not allowed,
or one of the directories in the path prefix
of pathname did not allow search permission.

EBUSY  pathname is currently in use by the system or some process that
prevents its  removal.   On  Linux  this  means
pathname is currently used as a mount point or is the root directory of
the calling process.

EFAULT pathname points outside your accessible address space.

EINVAL pathname has .  as last component.

ELOOP  Too many symbolic links were encountered in resolving pathname.

ENAMETOOLONG
pathname was too long.

ENOENT A directory component in pathname does not exist or is a dangling
symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOTDIR
pathname, or a component used as a directory in pathname, is not,
in fact, a directory.

ENOTEMPTY
pathname contains entries other than . and .. ; or, pathname has ..
as its final component.  POSIX.1-2001 also
allows EEXIST for this condition.

EPERM  The directory containing pathname has the sticky bit (S_ISVTX) set and
the process's effective user ID is  nei‐
ther  the  user  ID  of  the file to be deleted nor that of the
directory containing it, and the process is not
privileged (Linux: does not have the CAP_FOWNER capability).

EPERM  The filesystem containing pathname does not support the removal of
directories.

EROFS  pathname refers to a directory on a read-only filesystem.

 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), parent, true);
  int rc = 0;
  fuse_id id(req);
  // retrieve cap
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | X_OK | D_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    std::string sname = name;

    if (sname == ".") {
      rc = EINVAL;
    }

    if (sname.length() > 1024) {
      rc = ENAMETOOLONG;
    }

    fuse_ino_t del_ino = 0;

    if (!rc) {
      metad::shared_md md;
      metad::shared_md pmd;
      md = Instance().mds.lookup(req, parent, name);
      Track::Monitor mon(__func__, Instance().Tracker(), md->id(), true);
      XrdSysMutexHelper mLock(md->Locker());

      if (!md->id() || md->deleted()) {
	rc = ENOENT;
      }

      if ((!rc) && (!(md->mode() & S_IFDIR))) {
	rc = ENOTDIR;
      }

      eos_static_info("link=%d", md->nlink());

      if ((!rc) && (md->local_children().size() || md->nchildren())) {
	rc = ENOTEMPTY;
      }

      if (!rc) {
	pmd = Instance().mds.get(req, parent, pcap->authid());
	Instance().mds.remove(req, pmd, md, pcap->authid());
	del_ino = md->id();
      }
    }

    if (!rc) {
      if (Instance().Config().options.rmdir_is_sync) {
	Instance().mds.wait_deleted(req, del_ino);
      }
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc, name).c_str());
}

#ifdef _FUSE3
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rename(fuse_req_t req, fuse_ino_t parent, const char* name,
		fuse_ino_t newparent, const char* newname, unsigned int flags)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rename(fuse_req_t req, fuse_ino_t parent, const char* name,
		fuse_ino_t newparent, const char* newname)
/* -------------------------------------------------------------------------- */
#endif
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  // Need to pay attention to lock order here. This is the only (?) function where
  // we have to lock more than two inodes at the same time.
  //
  // Two racing requests with inverted source/target directories,
  // eg "mv dir1/file1 dir2/file2" and "mv dir2/file3 dir1/file4" can deadlock
  // us if we simply lock in order of source -> target.
  //
  // Instead, lock in order of increasing inode - both racing requests will
  // use the same locking order, and no deadlock can occur.
  fuse_ino_t first = std::min(parent, newparent);
  fuse_ino_t second = std::max(parent, newparent);
  Track::Monitor monp(__func__, Instance().Tracker(), first, true);
  Track::Monitor monn(__func__, Instance().Tracker(), second, true,
		      first == second);
  int rc = 0;
  fuse_id id(req);
  // do a parent check
  cap::shared_cap p1cap = Instance().caps.acquire(req, parent,
			  S_IFDIR | R_OK, true);
  cap::shared_cap p2cap = Instance().caps.acquire(req, newparent,
			  S_IFDIR | W_OK, true);

  if (p1cap->errc()) {
    rc = p1cap->errc();
  }

  if (!rc && p2cap->errc()) {
    rc = p2cap->errc();
  }

  if (!rc) {
    metad::shared_md md;
    metad::shared_md p1md;
    metad::shared_md p2md;
    md = Instance().mds.lookup(req, parent, name);
    p1md = Instance().mds.get(req, parent, p1cap->authid());
    p2md = Instance().mds.get(req, newparent, p2cap->authid());
    uint64_t md_ino = 0;
    {
      XrdSysMutexHelper mLock(md->Locker());

      if (md->deleted()) {
	// we need to wait that this entry is really gone
	Instance().mds.wait_flush(req, md);
      }

      if (!md->id() || md->deleted()) {
	rc = md->deleted() ? ENOENT : md->err();
      } else {
	md_ino = md->id();
      }
    }

    if (!rc) {
      Track::Monitor mone(__func__, Instance().Tracker(), md_ino, true);
      std::string new_name = newname;
      Instance().mds.mv(req, p1md, p2md, md, newname, p1cap->authid(),
			p2cap->authid());

      if (Instance().Config().options.rename_is_sync) {
	XrdSysMutexHelper mLock(md->Locker());
	Instance().mds.wait_flush(req, md);
      }
    }
  }

  EXEC_TIMING_END(__func__);
  fuse_reply_err(req, rc);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s target-name=%s", timing.RealTime(),
		    dump(id, parent, 0, rc, name).c_str(), newname);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::access(fuse_req_t req, fuse_ino_t ino, int mask)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  metad::shared_md md = Instance().mds.getlocal(req, ino);
  metad::shared_md pmd = md;
  mode_t mode = 0;
  mode_t pmode = mask;
  bool is_deleted = false;
  fuse_ino_t pino = 0;
  {
    XrdSysMutexHelper mLock(md->Locker());
    pino = (md->id() == 1) ? md->id() : md->pid();
    mode = md->mode();
    is_deleted = md->deleted();
  }
  pmode &= ~F_OK;

  if (!md->id()) {
    rc = is_deleted ? ENOENT : EIO;
  } else {
    if (S_ISREG(mode)) {
      pmd = Instance().mds.getlocal(req, pino);
    }

    if (!pmd->id()) {
      rc = EIO;
    } else {
      // we need a fresh cap for pino
      cap::shared_cap pcap = Instance().caps.acquire(req, pino,
			     S_IFDIR | pmode);
      XrdSysMutexHelper mLock(pcap->Locker());

      if (pcap->errc()) {
	rc = pcap->errc();

	if (rc == EPERM) {
	  rc = EACCES;
	}
      }
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("flags=%x sync=%d", fi->flags, (fi->flags & O_SYNC) ? 1 : 0);
  // FMODE_EXEC: "secret" internal flag which can be set only by the kernel when it's
  // reading a file destined to be used as an image for an execve.
#define FMODE_EXEC 0x20
  ExecveAlert execve(fi->flags & FMODE_EXEC);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  int rc = 0;
  fuse_id id(req);
  int mode = R_OK;

  if (fi->flags & (O_RDWR | O_WRONLY)) {
    mode = W_OK;
  }

  {
    metad::shared_md md;
    md = Instance().mds.get(req, ino);
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      // do a parent check
      cap::shared_cap pcap = Instance().caps.acquire(req, md->pid(),
			     S_IFDIR | mode);
      XrdSysMutexHelper capLock(pcap->Locker());

      if (pcap->errc()) {
	rc = pcap->errc();
      } else {
	uint64_t pquota = 0;

	if (mode == W_OK) {
	  if (!(pquota = Instance().caps.has_quota(pcap, 1024 * 1024))) {
	    rc = EDQUOT;
	  }
	}

	if (!rc) {
	  std::string md_name = md->name();
	  uint64_t md_ino = md->md_ino();
	  uint64_t md_pino = md->md_pino();
	  std::string cookie = md->Cookie();
	  capLock.UnLock();
	  struct fuse_entry_param e;
	  memset(&e, 0, sizeof(e));
	  md->convert(e);
	  mLock.UnLock();
	  data::data_fh* io = data::data_fh::Instance(Instance().datas.get(req, md->id(),
			      md), md, (mode == W_OK));
	  capLock.Lock(&pcap->Locker());
	  io->set_authid(pcap->authid());

	  if (pquota < pcap->max_file_size()) {
	    io->set_maxfilesize(pquota);
	  } else {
	    io->set_maxfilesize(pcap->max_file_size());
	  }

	  io->cap_ = pcap;
	  capLock.UnLock();
	  // attach a datapool object
	  fi->fh = (uint64_t) io;
	  io->ioctx()->set_remote(Instance().Config().hostport,
				  md_name,
				  md_ino,
				  md_pino,
				  req,
				  (mode == W_OK));
	  bool outdated = (io->ioctx()->attach(req, cookie, fi->flags) == EKEYEXPIRED);
	  fi->keep_cache = outdated ? 0 : Instance().Config().options.data_kernelcache;

	  if (md->creator()) {
	    fi->keep_cache = Instance().Config().options.data_kernelcache;
	  }

	  // files which have been broadcasted from a remote update are not cached during the first default:5 seconds
	  if ((time(NULL) - md->bc_time()) <
	      EosFuse::Instance().Config().options.nocache_graceperiod) {
	    fi->keep_cache = false;
	  }

	  fi->direct_io = 0;
	  eos_static_info("%s data-cache=%d", md->dump(e).c_str(), fi->keep_cache);
	}
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_open(req, fi);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, fi, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::mknod(fuse_req_t req, fuse_ino_t parent, const char* name,
	       mode_t mode, dev_t rdev)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);

  if (S_ISREG(mode) || S_ISFIFO(mode)) {
    create(req, parent, name, mode, 0);
  } else {
    rc = ENOSYS;
  }

  if (rc) {
    fuse_reply_err(req, rc);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc, name).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::create(fuse_req_t req, fuse_ino_t parent, const char* name,
		mode_t mode, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
/*
EACCES The  requested  access to the file is not allowed, or search permission is denied for one of the directories in
the path prefix of pathname, or the file did not exist yet and write access to  the  parent  directory  is  not
allowed.  (See also path_resolution(7).)

EDQUOT Where  O_CREAT  is  specified,  the  file  does not exist, and the user's quota of disk blocks or inodes on the
filesystem has been exhausted.

EEXIST pathname already exists and O_CREAT and O_EXCL were used.

EFAULT pathname points outside your accessible address space.

EFBIG  See EOVERFLOW.

EINTR  While blocked waiting to complete an open of a slow device (e.g., a FIFO; see fifo(7)),  the  call  was  inter‐
rupted by a signal handler; see signal(7).

EINVAL The filesystem does not support the O_DIRECT flag. See NOTES for more information.

EISDIR pathname refers to a directory and the access requested involved writing (that is, O_WRONLY or O_RDWR is set).

ELOOP  Too  many symbolic links were encountered in resolving pathname, or O_NOFOLLOW was specified but pathname was a
symbolic link.

EMFILE The process already has the maximum number of files open.

ENAMETOOLONG
pathname was too long.

ENFILE The system limit on the total number of open files has been reached.

ENODEV pathname refers to a device special file and no corresponding device exists.  (This is a Linux kernel  bug;  in
this situation ENXIO must be returned.)

ENOENT O_CREAT  is not set and the named file does not exist.  Or, a directory component in pathname does not exist or
is a dangling symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOSPC pathname was to be created but the device containing pathname has no room for the new file.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory,  or  O_DIRECTORY  was  specified  and
pathname was not a directory.

ENXIO  O_NONBLOCK  |  O_WRONLY is set, the named file is a FIFO and no process has the file open for reading.  Or, the
file is a device special file and no corresponding device exists.

EOVERFLOW
pathname refers to a regular file that is too large to be opened.  The usual scenario here is that an  applica‐
tion  compiled  on  a  32-bit  platform  without -D_FILE_OFFSET_BITS=64 tried to open a file whose size exceeds
(2<<31)-1 bits; see also O_LARGEFILE above.  This is the error specified by  POSIX.1-2001;  in  kernels  before
2.6.24, Linux gave the error EFBIG for this case.

EPERM  The  O_NOATIME  flag was specified, but the effective user ID of the caller did not match the owner of the file
and the caller was not privileged (CAP_FOWNER).

EROFS  pathname refers to a file on a read-only filesystem and write access was requested.

ETXTBSY
pathname refers to an executable image which is currently being executed and write access was requested.

EWOULDBLOCK
The O_NONBLOCK flag was specified, and an incompatible lease was held on the file (see fcntl(2)).
 */
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  Track::Monitor mon(__func__, Instance().Tracker(), parent, true);

  if (fi) {
    eos_static_debug("flags=%x", fi->flags);
  }

  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | W_OK, true);
  struct fuse_entry_param e;
  XrdSysMutexHelper capLock(pcap->Locker());

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    capLock.UnLock();
    {
      if (!Instance().caps.has_quota(pcap, 1024 * 1024)) {
	rc = EDQUOT;
      }
    }

    if (!rc) {
      metad::shared_md md;
      metad::shared_md pmd;
      md = Instance().mds.lookup(req, parent, name);
      pmd = Instance().mds.get(req, parent, pcap->authid());
      {
	uint64_t del_ino = 0;
	// logic avoiding a create/unlink/create sync/async race
	{
	  XrdSysMutexHelper pLock(pmd->Locker());
	  auto it = pmd->get_todelete().find(name);

	  if ((it != pmd->get_todelete().end()) && it->second) {
	    del_ino = it->second;
	  }
	}

	if (del_ino) {
	  Instance().mds.wait_deleted(req, del_ino);
	}
      }
      XrdSysMutexHelper mLock(md->Locker());

      if (md->id() && !md->deleted()) {
	rc = EEXIST;
      } else {
	if (md->deleted()) {
	  // we need to wait that this entry is really gone
	  Instance().mds.wait_flush(req, md);
	}

	md->set_err(0);
	md->set_mode(mode | (S_ISFIFO(mode) ? S_IFIFO : S_IFREG));

	if (S_ISFIFO(mode)) {
	  (*md->mutable_attr())[k_fifo] = "";
	}

	struct timespec ts;

	eos::common::Timing::GetTimeSpec(ts);

	md->set_name(name);

	md->set_atime(ts.tv_sec);

	md->set_atime_ns(ts.tv_nsec);

	md->set_mtime(ts.tv_sec);

	md->set_mtime_ns(ts.tv_nsec);

	md->set_ctime(ts.tv_sec);

	md->set_ctime_ns(ts.tv_nsec);

	md->set_btime(ts.tv_sec);

	md->set_btime_ns(ts.tv_nsec);

	// need to update the parent mtime
	md->set_pmtime(ts.tv_sec);

	md->set_pmtime_ns(ts.tv_nsec);

	md->set_uid(pcap->uid());

	md->set_gid(pcap->gid());

	md->set_id(Instance().mds.insert(req, md, pcap->authid()));

	md->set_nlink(1);

	md->set_creator(true);

	// avoid lock-order violation
	{
	  mLock.UnLock();
	  XrdSysMutexHelper mLockParent(pmd->Locker());
	  pmd->set_mtime(ts.tv_sec);
	  pmd->set_mtime_ns(ts.tv_nsec);

	  // get file inline size from parent attribute
	  if (pmd->attr().count("sys.file.inline.maxsize")) {
	    auto maxsize = (*pmd->mutable_attr())["sys.file.inline.maxsize"];
	    md->set_inlinesize(strtoull(maxsize.c_str(), 0, 10));
	  }

	  mLockParent.UnLock();
	  mLock.Lock(&md->Locker());
	}

	if ((Instance().Config().options.create_is_sync) ||
	    (fi && fi->flags & O_EXCL)) {
	  md->set_type(md->EXCL);
	  rc = Instance().mds.add_sync(req, pmd, md, pcap->authid());
	  md->set_type(md->MD);
	} else {
	  Instance().mds.add(req, pmd, md, pcap->authid());
	}

	memset(&e, 0, sizeof(e));

	if (!rc) {
	  Instance().caps.book_inode(pcap);
	  md->convert(e);
	  md->lookup_inc();

	  if (fi) {
	    // -----------------------------------------------------------------------
	    // FUSE caches the file for reads on the same filedescriptor in the buffer
	    // cache, but the pages are released once this filedescriptor is released.
	    fi->keep_cache = Instance().Config().options.data_kernelcache;

	    if ((fi->flags & O_DIRECT) ||
		(fi->flags & O_SYNC)) {
	      fi->direct_io = 1;
	    } else {
	      fi->direct_io = 0;
	    }

	    std::string md_name = md->name();
	    uint64_t md_ino = md->md_ino();
	    uint64_t md_pino = md->md_pino();
	    std::string cookie = md->Cookie();
	    mLock.UnLock();
	    data::data_fh* io = data::data_fh::Instance(Instance().datas.get(req, md->id(),
				md), md, true);
	    io->set_authid(pcap->authid());
	    io->set_maxfilesize(pcap->max_file_size());
	    io->cap_ = pcap;
	    // attach a datapool object
	    fi->fh = (uint64_t) io;
	    io->ioctx()->set_remote(Instance().Config().hostport,
				    md_name,
				    md_ino,
				    md_pino,
				    req,
				    true);
	    io->ioctx()->attach(req, cookie, fi->flags);
	  }
	}

	eos_static_info("%s", md->dump(e).c_str());
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    if (fi)
      // create
    {
      fuse_reply_create(req, &e, fi);
    } else
      // mknod
    {
      fuse_reply_entry(req, &e);
    }
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
	      struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  eos_static_debug("inode=%llu size=%li off=%llu",
		   (unsigned long long) ino, size, (unsigned long long) off);
  eos_static_debug("");
  fuse_id id(req);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  data::data_fh* io = (data::data_fh*) fi->fh;
  ssize_t res = 0;
  int rc = 0;

  if (io) {
    char* buf = 0;

    if ((res = io->ioctx()->peek_pread(req, buf, size, off)) == -1) {
      rc = errno ? errno : EIO;
    } else {
      fuse_reply_buf(req, buf, res);
    }

    io->ioctx()->release_pread();
  } else {
    rc = ENXIO;
  }

  if (rc) {
    fuse_reply_err(req, rc);
  }

  eos_static_debug("t(ms)=%.03f %s", timing.RealTime(),
		   dump(id, ino, 0, rc).c_str());
  EXEC_TIMING_END(__func__);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size,
	       off_t off, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  eos_static_debug("inode=%lld size=%lld off=%lld buf=%lld",
		   (long long) ino, (long long) size,
		   (long long) off, (long long) buf);
  eos_static_debug("");
  fuse_id id(req);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  data::data_fh* io = (data::data_fh*) fi->fh;
  int rc = 0;

  if (io) {
    eos_static_debug("max-file-size=%llu", io->maxfilesize());

    if ((off + size) > io->maxfilesize()) {
      eos_static_err("io-error: maximum file size exceeded inode=%lld size=%lld off=%lld buf=%lld max-size=%llu",
		     ino, size, off, buf, io->maxfilesize());
      rc = EFBIG;
    } else {
      if (!EosFuse::instance().getCap().has_quota(io->cap_, size)) {
	eos_static_err("quota-error: inode=%lld size=%lld off=%lld buf=%lld", ino, size,
		       off, buf);
	rc = EDQUOT;
      } else {
	if (io->ioctx()->pwrite(req, buf, size, off) == -1) {
	  eos_static_err("io-error: inode=%lld size=%lld off=%lld buf=%lld errno=%d", ino,
			 size,
			 off, buf, errno);
	  rc = errno ? errno : EIO;
	} else {
	  {
	    XrdSysMutexHelper mLock(io->mdctx()->Locker());
	    io->mdctx()->set_size(io->ioctx()->size());
	    io->set_update();
	  }
	  fuse_reply_write(req, size);
	}
      }
    }
  } else {
    rc = ENXIO;
  }

  if (rc) {
    fuse_reply_err(req, rc);
  }

  eos_static_debug("t(ms)=%.03f %s", timing.RealTime(),
		   dump(id, ino, 0, rc).c_str());
  EXEC_TIMING_END(__func__);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  int rc = 0;
  fuse_id id(req);

  if (fi->fh) {
    data::data_fh* io = (data::data_fh*) fi->fh;
    std::string cookie = "";
    io->ioctx()->detach(req, cookie, io->rw);
    delete io;
    Instance().datas.release(req, ino);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  fuse_reply_err(req, rc);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
	       struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("datasync=%d", datasync);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  data::data_fh* io = (data::data_fh*) fi->fh;

  if (io) {
    {
      std::string fname = "";
      {
	XrdSysMutexHelper mLock(io->md->Locker());
	fname = io->md->name();
      }

      if (filename::matches_suffix(fname,
				   Instance().Config().options.no_fsync_suffixes)) {
	if (EOS_LOGS_DEBUG) {
	  eos_static_info("name=%s is in no-fsync list - suppressing fsync call",
			  fname.c_str());
	}
      } else {
	if (Instance().Config().options.global_flush) {
	  Instance().mds.begin_flush(req, io->md,
				     io->authid()); // flag an ongoing flush centrally
	}

	struct timespec tsnow;

	eos::common::Timing::GetTimeSpec(tsnow);

	XrdSysMutexHelper mLock(io->md->Locker());

	io->md->set_mtime(tsnow.tv_sec);

	if (!rc) {
	  // step 2 call sync - this currently flushed all open filedescriptors - should be ok
	  rc = io->ioctx()->sync(); // actually wait for writes to be acknowledged
	  rc = rc ? (errno ? errno : EIO) : 0;
	} else {
	  rc = errno ? errno : EIO;
	}

	if (Instance().Config().options.global_flush) {
	  Instance().mds.end_flush(req, io->md,
				   io->authid()); // unflag an ongoing flush centrally
	}
      }
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("ino=%#lx nlookup=%d", ino, nlookup);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  int rc = 0;
  fuse_id id(req);
  rc = Instance().mds.forget(req, ino, nlookup);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s nlookup=%d", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str(), nlookup);

  if (!rc) {
    Instance().Tracker().forget(ino);
  }

  fuse_reply_none(req);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  int rc = 0;
  fuse_id id(req);
  data::data_fh* io = (data::data_fh*) fi->fh;
  bool invalidate_inode = false;

  if (io) {
    if (io->has_update()) {
      cap::shared_cap pcap = Instance().caps.acquire(req, io->md->pid(),
			     S_IFDIR | W_OK, true);
      XrdSysMutexHelper pLock(pcap->Locker());

      if (pcap->errc()) {
	rc = pcap->errc();
      } else {
	{
	  ssize_t size_change = (int64_t) io->md->size() - (int64_t) io->opensize();

	  if (size_change > 0) {
	    Instance().caps.book_volume(pcap, size_change);
	  } else {
	    Instance().caps.free_volume(pcap, size_change);
	  }

	  eos_static_debug("booking %ld bytes on cap ", size_change);
	}
	pLock.UnLock();
	struct timespec tsnow;
	eos::common::Timing::GetTimeSpec(tsnow);

	// possibly inline the file in extended attribute before mds update
	if (io->ioctx()->inline_file()) {
	  eos_static_debug("file is inlined");
	} else {
	  eos_static_debug("file is not inlined");
	}

	XrdSysMutexHelper mLock(io->md->Locker());
	io->md->set_mtime(tsnow.tv_sec);
	io->md->set_mtime_ns(tsnow.tv_nsec);

	// actually do the flush
	if ((rc = io->ioctx()->flush(req))) {
	  invalidate_inode = true;
	  io->md->set_size(io->opensize());
	} else {
	  // if we have a flush error, we don't update the MD record
	  Instance().mds.update(req, io->md, io->authid());
	}

	std::string cookie = io->md->Cookie();
	io->ioctx()->store_cookie(cookie);
	pcap->Locker().Lock();

	if (!Instance().caps.has_quota(pcap, 0)) {
	  // we signal an error to the client if the quota get's exceeded although
	  // we let the file be complete
	  rc = EDQUOT;
	}

	pcap->Locker().UnLock();
      }
    }

    // unlock all locks for that owner
    struct flock lock;
    lock.l_type = F_UNLCK;
    lock.l_start = 0;
    lock.l_len = -1;
    lock.l_pid = fi->lock_owner;
    rc |= Instance().mds.setlk(req, io->mdctx(), &lock, 0);
  }

  fuse_reply_err(req, rc);

  if (invalidate_inode) {
    eos_static_warning("invalidating ino=%#lx after flush error", ino);
    kernelcache::inval_inode(ino, true);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

#ifdef __APPLE__
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getxattr(fuse_req_t req, fuse_ino_t ino, const char* xattr_name,
		  size_t size, uint32_t position)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getxattr(fuse_req_t req, fuse_ino_t ino, const char* xattr_name,
		  size_t size)
/* -------------------------------------------------------------------------- */
#endif
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  cap::shared_cap pcap;
  std::string key = xattr_name;
  std::string value;
  bool local_getxattr = false;

  // the root user has a bypass to be able to retrieve information in
  // realtime
  if (fuse_req_ctx(req)->uid == 0) {
    static std::string s_md = "system.eos.md";
    static std::string s_cap = "system.eos.cap";
    static std::string s_ls_caps = "system.eos.caps";
    static std::string s_ls_vmap = "system.eos.vmap";

    if (key.substr(0, s_md.length()) == s_md) {
      local_getxattr = true;
      metad::shared_md md;
      pcap = Instance().caps.get(req, ino);
      md = Instance().mds.get(req, ino, pcap->authid());
      {
	value = Instance().mds.dump_md(md);
      }
    }

    if (key.substr(0, s_cap.length()) == s_cap) {
      local_getxattr = true;
      pcap = Instance().caps.get(req, ino);
      {
	value = pcap->dump();
      }
    }

    if (key.substr(0, s_ls_caps.length()) == s_ls_caps) {
      local_getxattr = true;
      value = Instance().caps.ls();
    }

    if (key.substr(0, s_ls_vmap.length()) == s_ls_vmap) {
      local_getxattr = true;
      value = Instance().mds.vmaps().dump();
    }

    if ((size) && (value.size() > size)) {
      value.erase(size - 4);
      value += "...";
    }
  }

  if (!local_getxattr) {
    {
      metad::shared_md md;
      metad::shared_md pmd;
      static std::string s_sec = "security.";
      static std::string s_acl_a = "system.posix_acl_access";
      static std::string s_acl_d = "system.posix_acl_default";
      static std::string s_apple = "com.apple";
      static std::string s_racl = "system.richacl";

      // don't return any security attribute
      if (key.substr(0, s_sec.length()) == s_sec) {
	rc = ENOATTR;
      } else {
	// don't return any posix acl attribute
	if ((key == s_acl_a) || (key == s_acl_d)) {
	  rc = ENOATTR;
	}

#ifdef __APPLE__
	else

	  // don't return any finder attribute
	  if (key.substr(0, s_apple.length()) == s_apple) {
	    rc = ENOATTR;
	  }

#endif
      }

      if (!rc) {
	md = Instance().mds.get(req, ino);
	XrdSysMutexHelper mLock(md->Locker());

	if (!md->id() || md->deleted()) {
	  rc = md->deleted() ? ENOENT : md->err();
	} else {
	  auto map = md->attr();

	  if (key.substr(0, 4) == "eos.") {
	    if (key == "eos.md_ino") {
	      std::string md_ino;
	      value = eos::common::StringConversion::GetSizeString(md_ino,
		      (unsigned long long) md->md_ino());
	    }

	    if (key == "eos.btime") {
	      char btime[256];
	      snprintf(btime, sizeof(btime), "%lu.%lu", md->btime(), md->btime_ns());
	      value = btime;
	      ;
	    }

	    if (key == "eos.name") {
	      value = Instance().Config().name;
	    }

	    if (key == "eos.hostport") {
	      value = Instance().Config().hostport;
	    }

	    if (key == "eos.mgmurl") {
	      std::string mgmurl = "root://";
	      mgmurl += Instance().Config().hostport;
	      value = mgmurl;
	    }

	    if (key == "eos.quota") {
	      pcap = Instance().caps.acquire(req, ino,
					     R_OK);

	      if (pcap->errc()) {
		rc = pcap->errc();
	      } else {
		cap::shared_quota q = Instance().caps.quota(pcap);
		XrdSysMutexHelper qLock(q->Locker());
		char qline[1024];
		snprintf(qline, sizeof(qline),
			 "instance             uid     gid        vol-avail        ino-avail        max-fsize                         endpoint\n"
			 "%-16s %7u %7u %16lu %16lu %16lu %32s\n",
			 Instance().Config().name.c_str(),
			 pcap->uid(),
			 pcap->gid(),
			 q->volume_quota(),
			 q->inode_quota(),
			 pcap->max_file_size(),
			 Instance().Config().hostport.c_str());
		value = qline;
	      }
	    }
	  } else {
	    if (S_ISDIR(md->mode())) {
	      // retrieve the appropriate cap of this inode
	      pcap = Instance().caps.acquire(req, ino,
					     R_OK);
	    } else {
	      // retrieve the appropriate cap of the parent inode
	      pcap = Instance().caps.acquire(req, md->pid(), R_OK);
	    }

	    if (pcap->errc()) {
	      rc = pcap->errc();
	    } else {
#ifdef RICHACL_FOUND

	      if (key == s_racl) {
		if (map.count("sys.eval.useracl") == 0 || map.count("user.acl") == 0 ||
		    map["user.acl"].length() == 0) {
		  rc = ENOATTR;
		} else {
		  const char* eosacl = map["user.acl"].c_str();
		  eos_static_debug("eosacl '%s'", eosacl);
		  struct richacl* a = eos2racl(eosacl, md->mode());

		  if (a != NULL) {
		    size_t sz = richacl_xattr_size(a);
#if 1
		    value.assign(sz, '\0'); /* allocate and clear result buffer */
		    richacl_to_xattr(a, (void*) value.c_str());
#else
		    //void *a_x = (void *) alloca(sz);
		    //richacl_to_xattr(a, a_x);
		    //value.assign((char *)a_x, sz);
		    //free(a_x);
#endif
		    char* a_t = richacl_to_text(a, 0);
		    eos_static_debug("eos2racl returned raw size %d, decoded: %s", sz, a_t);
		    free(a_t);
		    richacl_free(a);
		  } else { /* unsupported EOS Acl */
		    size_t xx = 0;
		    value.assign((char*) &xx, sizeof(xx));  /* Invalid xattr */
		  }

		  if (EOS_LOGS_DEBUG) {
		    eos_static_debug("racl getxattr %d: %s", value.length(),
				     escape(value).c_str());
		  }
		}
	      } else
#endif /*RICHACL_FOUND*/
		if (!map.count(key)) {
		  rc = ENOATTR;
		} else {
		  value = map[key];
		}
	    }

	    if (size != 0) {
	      if (value.size() > size) {
		rc = ERANGE;
	      }
	    }
	  }
	}
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    if (size == 0) {
      fuse_reply_xattr(req, value.size());
    } else {
      fuse_reply_buf(req, value.c_str(), value.size());
    }
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc, xattr_name).c_str());
}

#ifdef __APPLE__
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setxattr(fuse_req_t req, fuse_ino_t ino, const char* xattr_name,
		  const char* xattr_value, size_t size, int flags,
		  uint32_t position)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setxattr(fuse_req_t req, fuse_ino_t ino, const char* xattr_name,
		  const char* xattr_value, size_t size, int flags)
/* -------------------------------------------------------------------------- */
#endif
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("key=%s", xattr_name);
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  int rc = 0;
  fuse_id id(req);
  cap::shared_cap pcap;
  std::string value;
  bool local_setxattr = false;
  std::string key = xattr_name;
  value.assign(xattr_value, size);
#ifdef RICHACL_FOUND

  if (EOS_LOGS_DEBUG) {
    eos_static_debug("value: '%s' l=%d", escape(value).c_str(), size);
  }

#endif /*RICHACL_FOUND*/
  // the root user has a bypass to be able to change th fuse configuration in
  // realtime
  {
    static std::string s_debug = "system.eos.debug";
    static std::string s_dropcap = "system.eos.dropcap";
    static std::string s_dropallcap = "system.eos.dropallcap";

    if (key.substr(0, s_debug.length()) == s_debug) {
      local_setxattr = true;
      // only root can do this configuration changes

      if (fuse_req_ctx(req)->uid == 0) {
	rc = EINVAL;
#ifdef EOSCITRINE

	if (value == "notice") {
	  eos::common::Logging::GetInstance().SetLogPriority(LOG_NOTICE);
	  rc = 0;
	}

	if (value == "info") {
	  eos::common::Logging::GetInstance().SetLogPriority(LOG_INFO);
	  rc = 0;
	}

	if (value == "debug") {
	  eos::common::Logging::GetInstance().SetLogPriority(LOG_DEBUG);
	  rc = 0;
	}

#else

	if (value == "notice") {
	  eos::common::Logging::SetLogPriority(LOG_NOTICE);
	  rc = 0;
	}

	if (value == "info") {
	  eos::common::Logging::SetLogPriority(LOG_INFO);
	  rc = 0;
	}

	if (value == "debug") {
	  eos::common::Logging::SetLogPriority(LOG_DEBUG);
	  rc = 0;
	}

#endif
      } else {
	rc = EPERM;
      }
    }

    if (key.substr(0, s_dropcap.length()) == s_dropcap) {
      local_setxattr = true;
      cap::shared_cap pcap = Instance().caps.get(req, ino);

      if (pcap->id()) {
	Instance().caps.forget(pcap->capid(req, ino));
      }
    }

    if (key.substr(0, s_dropallcap.length()) == s_dropallcap) {
      local_setxattr = true;

      if (fuse_req_ctx(req)->uid == 0) {
	Instance().caps.reset();
      } else {
	rc = EPERM;
      }
    }
  }

  if (!local_setxattr) {
    metad::shared_md md;
    md = Instance().mds.get(req, ino);
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      // retrieve the appropriate cap
      if (S_ISDIR(md->mode())) {
	pcap = Instance().caps.acquire(req, ino,
				       SA_OK);
      } else {
	pcap = Instance().caps.acquire(req, md->pid(),
				       SA_OK);
      }

      if (pcap->errc()) {
	rc = pcap->errc();
      } else {
	static std::string s_sec = "security.";
	static std::string s_acl = "system.posix_acl_access";
	static std::string s_apple = "com.apple";
	static std::string s_racl = "system.richacl";

	// ignore silently any security attribute
	if (key.substr(0, s_sec.length()) == s_sec) {
	  rc = 0;
	} else

	  // ignore silently any posix acl attribute
	  if (key == s_acl) {
	    rc = 0;
	  }

#ifdef __APPLE__
	  else

	    // ignore silently any finder attribute
	    if (key.substr(0, s_apple.length()) == s_apple) {
	      rc = 0;
	    }

#endif
#ifdef RICHACL_FOUND
	    else if (key == s_racl) {
	      struct richacl* a = richacl_from_xattr(xattr_value, size);
	      char* a_t = richacl_to_text(a, 0);
	      eos_static_debug("acl a_t '%s'", a_t);
	      free(a_t);
	      char eosAcl[512];
	      racl2eos(a, eosAcl, sizeof(eosAcl));
	      eos_static_debug("acl eosacl '%s'", eosAcl);
	      auto map = md->mutable_attr();

	      if (!map->count("sys.eval.useracl")) {
		// in case user acls are disabled
		rc = EPERM;
	      } else {
		(*map)["user.acl"] = std::string(eosAcl);
		Instance().mds.update(req, md, pcap->authid());
	      }
	    }

#endif /*RICHACL_FOUND*/
	    else {
	      auto map = md->mutable_attr();
	      bool exists = false;

	      if ((*map).count(key)) {
		exists = true;
	      }

	      if (exists && (flags == XATTR_CREATE)) {
		rc = EEXIST;
	      } else if (!exists && (flags == XATTR_REPLACE)) {
		rc = ENOATTR;
	      } else {
		(*map)[key] = value;
		Instance().mds.update(req, md, pcap->authid());
	      }
	    }
      }
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  cap::shared_cap pcap;
  std::string attrlist;
  size_t attrlistsize = 0;
  metad::shared_md md;
  md = Instance().mds.get(req, ino);

  // retrieve the appropriate cap
  if (S_ISDIR(md->mode())) {
    pcap = Instance().caps.acquire(req, ino,
				   SA_OK, true);
  } else {
    pcap = Instance().caps.acquire(req, md->pid(),
				   SA_OK, true);
  }

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      auto map = md->attr();
      size_t attrlistsize = 0;
      attrlist = "";

      for (auto it = map.begin(); it != map.end(); ++it) {
	attrlistsize += it->first.length() + 1;
	attrlist += it->first;
	attrlist += '\0';
      }

      if (size != 0) {
	if (attrlist.size() > size) {
	  rc = ERANGE;
	}
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    if (size == 0) {
      fuse_reply_xattr(req, attrlistsize);
    } else {
      fuse_reply_buf(req, attrlist.c_str(), attrlist.length());
    }
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::removexattr(fuse_req_t req, fuse_ino_t ino, const char* xattr_name)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  fuse_id id(req);
  cap::shared_cap pcap;
  metad::shared_md md;
  md = Instance().mds.get(req, ino);

  // retrieve the appropriate cap
  if (S_ISDIR(md->mode())) {
    pcap = Instance().caps.acquire(req, ino,
				   SA_OK, true);
  } else {
    pcap = Instance().caps.acquire(req, md->pid(),
				   SA_OK, true);
  }

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = md->deleted() ? ENOENT : md->err();
    } else {
      std::string key = xattr_name;
      static std::string s_sec = "security.";
      static std::string s_acl = "system.posix_acl";
      static std::string s_apple = "com.apple";
      static std::string s_racl = "system.richacl";

      // ignore silently any security attribute
      if (key.substr(0, s_sec.length()) == s_sec) {
	rc = 0;
      } else

	// ignore silently any posix acl attribute
	if (key == s_acl) {
	  rc = 0;
	}

#ifdef __APPLE__
	else

	  // ignore silently any finder attribute
	  if (key.substr(0, s_apple.length()) == s_apple) {
	    rc = 0;
	  }

#endif
	  else {
#ifdef RICHACL_FOUND

	    if (key == s_racl) {
	      key = "user.acl";
	    }

#endif
	    auto map = md->mutable_attr();
	    bool exists = false;

	    if ((*map).count(key)) {
	      exists = true;
	    }

	    if (!exists) {
	      rc = ENOATTR;
	    } else {
	      (*map).erase(key);
	      Instance().mds.update(req, md, pcap->authid());
	    }
	  }
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::readlink(fuse_req_t req, fuse_ino_t ino)
/* -------------------------------------------------------------------------- */
/*
 EACCES Search permission is denied for a component of the path prefix.  (See also path_resolution(7).)

 EFAULT buf extends outside the process’s allocated address space.

 EINVAL bufsiz is not positive.

 EINVAL The named file is not a symbolic link.

 EIO    An I/O error occurred while reading from the file system.

 ELOOP  Too many symbolic links were encountered in translating the pathname.

 ENAMETOOLONG
	A pathname, or a component of a pathname, was too long.

 ENOENT The named file does not exist.

 ENOMEM Insufficient kernel memory was available.

 ENOTDIR
	A component of the path prefix is not a directory.
 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  int rc = 0;
  std::string target;
  fuse_id id(req);
  cap::shared_cap pcap;
  metad::shared_md md;
  md = Instance().mds.get(req, ino);
  pcap = Instance().caps.acquire(req, md->pid(),
				 R_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted()) {
      rc = ENOENT;
    } else {
      if (!(md->mode() & S_IFLNK)) {
	// no a link
	rc = EINVAL;
      } else {
	target = md->target();
      }
    }
  }

  if (target.substr(0, 6) == "mount:") {
    std::string env;

    // if not shared, set the caller credentials
    if (0) {
      env = fusexrdlogin::environment(req);
    }

    std::string localpath = Instance().Prefix(Instance().mds.calculateLocalPath(
			      md));
    rc = Instance().Mounter().mount(target, localpath, env);
  }

  if (target.substr(0, 11) == "squashfuse:") {
    std::string env;
    //    env = fusexrdlogin::environment(req);
    std::string localpath = Instance().Prefix(Instance().mds.calculateLocalPath(
			      md));
    rc = Instance().Mounter().squashfuse(target, localpath, env);
  }

  if (!rc) {
    fuse_reply_readlink(req, target.c_str());
    return;
  } else {
    fuse_reply_err(req, errno);
    return;
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::symlink(fuse_req_t req, const char* link, fuse_ino_t parent,
		 const char* name)
/* -------------------------------------------------------------------------- */
/*
 EACCES Write access to the directory containing newpath is denied, or one of the directories in the path
	prefix of newpath did not allow search permission.  (See also path_resolution(7).)

 EEXIST newpath already exists.

 EFAULT oldpath or newpath points outside your accessible address space.

 EIO    An I/O error occurred.

 ELOOP  Too many symbolic links were encountered in resolving newpath.

 ENAMETOOLONG
	oldpath or newpath was too long.

 ENOENT A directory component in newpath does not exist or is a dangling symbolic link, or oldpath is the
	empty string.

 ENOMEM Insufficient kernel memory was available.

 ENOSPC The device containing the file has no room for the new directory entry.

 ENOTDIR
	A component used as a directory in newpath is not, in fact, a directory.

 EPERM  The file system containing newpath does not support the creation of symbolic links.

 EROFS  newpath is on a read-only file system.

 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), parent);
  int rc = 0;
  fuse_id id(req);
  struct fuse_entry_param e;
  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | W_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    metad::shared_md md;
    metad::shared_md pmd;
    md = Instance().mds.lookup(req, parent, name);
    pmd = Instance().mds.get(req, parent, pcap->authid());
    XrdSysMutexHelper mLock(md->Locker());

    if (md->id() && !md->deleted()) {
      rc = EEXIST;
    } else {
      if (md->deleted()) {
	// we need to wait that this entry is really gone
	Instance().mds.wait_flush(req, md);
      }

      md->set_mode(S_IRWXU | S_IRWXG | S_IRWXO | S_IFLNK);
      md->set_target(link);
      md->set_err(0);
      struct timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      md->set_name(name);
      md->set_atime(ts.tv_sec);
      md->set_atime_ns(ts.tv_nsec);
      md->set_mtime(ts.tv_sec);
      md->set_mtime_ns(ts.tv_nsec);
      md->set_ctime(ts.tv_sec);
      md->set_ctime_ns(ts.tv_nsec);
      md->set_btime(ts.tv_sec);
      md->set_btime_ns(ts.tv_nsec);
      md->set_uid(pcap->uid());
      md->set_gid(pcap->gid());
      md->set_id(Instance().mds.insert(req, md, pcap->authid()));
      md->lookup_inc();

      if (Instance().Config().options.symlink_is_sync) {
	md->set_type(md->EXCL);
	rc = Instance().mds.add_sync(req, pmd, md, pcap->authid());
	md->set_type(md->MD);
      } else {
	Instance().mds.add(req, pmd, md, pcap->authid());
      }

      memset(&e, 0, sizeof(e));
      md->convert(e);
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_entry(req, &e);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, parent, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t parent,
	      const char* newname)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  if (EOS_LOGS_DEBUG) {
    eos_static_debug("hlnk newname=%s ino=%#lx parent=%#lx", newname, ino, parent);
  }

  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), parent);
  int rc = 0;
  fuse_id id(req);
  struct fuse_entry_param e;
  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
			 S_IFDIR | W_OK, true);

  if (pcap->errc()) {
    rc = pcap->errc();
  } else {
    metad::shared_md md; /* the new name */
    metad::shared_md pmd; /* the parent directory for the new name */
    metad::shared_md tmd; /* the link target */
    md = Instance().mds.lookup(req, parent, newname);
    pmd = Instance().mds.get(req, parent, pcap->authid());
    XrdSysMutexHelper mLock(md->Locker());

    if (md->id() && !md->deleted()) {
      rc = EEXIST;
    } else {
      if (md->deleted()) {
	// we need to wait that this entry is really gone
	Instance().mds.wait_flush(req, md);
      }

      tmd = Instance().mds.get(req, ino, pcap->authid()); /* link target */

      if (tmd->id() == 0 || tmd->deleted()) {
	rc = ENOENT;
      } else if (tmd->pid() != parent) {
	rc = EXDEV; /* only same parent supported */
      } else {
	XrdSysMutexHelper tmLock(tmd->Locker());

	if (EOS_LOGS_DEBUG) {
	  eos_static_debug("hlnk tmd id=%ld %s", tmd->id(), tmd->name().c_str());
	}

	md->set_mode(tmd->mode());
	md->set_err(0);
	struct timespec ts;
	eos::common::Timing::GetTimeSpec(ts);
	md->set_name(newname);
	char tgtStr[64];
	snprintf(tgtStr, sizeof(tgtStr), "////hlnk%ld",
		 tmd->md_ino()); /* This triggers the hard link and specifies the target inode */
	md->set_target(tgtStr);
	md->set_atime(tmd->atime());
	md->set_atime_ns(tmd->atime_ns());
	md->set_mtime(tmd->mtime());
	md->set_mtime_ns(tmd->mtime_ns());
	md->set_ctime(tmd->ctime());
	md->set_ctime_ns(tmd->ctime_ns());
	md->set_btime(tmd->btime());
	md->set_btime_ns(tmd->btime_ns());
	md->set_uid(tmd->uid());
	md->set_gid(tmd->gid());
	md->set_size(tmd->size());
	// increase the link count of the target
	auto attrMap = tmd->attr();
	size_t nlink = 1;

	if (attrMap.count(k_nlink)) {
	  nlink += std::stol(attrMap[k_nlink]);
	}

	auto wAttrMap = tmd->mutable_attr();
	(*wAttrMap)[k_nlink] = std::to_string(nlink);
	eos_static_debug("setting link count to %d", nlink);
	auto sAttrMap = md->mutable_attr();
	(*sAttrMap)[k_mdino] = std::to_string(tmd->md_ino());
	tmd->set_nlink(nlink + 1);
	tmd->Locker().UnLock();
	md->set_id(Instance().mds.insert(req, md, pcap->authid()));
	rc = Instance().mds.add_sync(req, pmd, md, pcap->authid());
	md->set_target("");
	md->Locker().UnLock();

	if (!rc) {
	  XrdSysMutexHelper tmLock(tmd->Locker());
	  memset(&e, 0, sizeof(e));
	  tmd->convert(e);

	  if (EOS_LOGS_DEBUG) {
	    eos_static_debug("hlnk tmd %s %s", tmd->name().c_str(), tmd->dump(e).c_str());
	  }

	  // reply with the target entry
	  fuse_reply_entry(req, &e);
	}
      }
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
}

void
/* -------------------------------------------------------------------------- */
EosFuse::getlk(fuse_req_t req, fuse_ino_t ino,
	       struct fuse_file_info* fi, struct flock* lock)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino);
  fuse_id id(req);
  int rc = 0;

  if (!Instance().Config().options.global_locking) {
    // use default local locking
    rc = EOPNOTSUPP;
  } else {
    // use global locking
    data::data_fh* io = (data::data_fh*) fi->fh;

    if (io) {
      rc = Instance().mds.getlk(req, io->mdctx(), lock);
    } else {
      rc = ENXIO;
    }
  }

  if (rc) {
    fuse_reply_err(req, rc);
  } else {
    fuse_reply_lock(req, lock);
  }

  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::setlk(fuse_req_t req, fuse_ino_t ino,
	       struct fuse_file_info* fi,
	       struct flock* lock, int sleep)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);
  eos_static_debug("");
  ADD_FUSE_STAT(__func__, req);
  EXEC_TIMING_BEGIN(__func__);
  Track::Monitor mon(__func__, Instance().Tracker(), ino, true);
  fuse_id id(req);
  int rc = 0;

  if (!Instance().Config().options.global_locking) {
    // use default local locking
    rc = EOPNOTSUPP;
  } else {
    // use global locking
    data::data_fh* io = (data::data_fh*) fi->fh;

    if (io) {
      size_t w_ms = 10;

      do {
	// we currently implement the polling lock on client side due to the
	// thread-per-link model of XRootD
	rc = Instance().mds.setlk(req, io->mdctx(), lock, sleep);

	if (rc && sleep) {
	  std::this_thread::sleep_for(std::chrono::milliseconds(w_ms));
	  // do exponential back-off with a hard limit at 1s
	  w_ms *= 2;

	  if (w_ms > 1000) {
	    w_ms = 1000;
	  }

	  continue;
	}

	break;
      } while (rc);
    } else {
      rc = ENXIO;
    }
  }

  fuse_reply_err(req, rc);
  EXEC_TIMING_END(__func__);
  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
		    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
EosFuse::getHbStat(eos::fusex::statistics& hbs)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("get statistics");
  eos::common::LinuxStat::linux_stat_t osstat;
#ifndef __APPLE__
  eos::common::LinuxMemConsumption::linux_mem_t mem;

  if (!eos::common::LinuxMemConsumption::GetMemoryFootprint(mem)) {
    eos_static_err("failed to get the MEM usage information");
  }

  if (!eos::common::LinuxStat::GetStat(osstat)) {
    eos_static_err("failed to get the OS usage information");
  }

#endif
  hbs.set_inodes(getMdStat().inodes());
  hbs.set_inodes_todelete(getMdStat().inodes_deleted());
  hbs.set_inodes_backlog(getMdStat().inodes_backlog());
  hbs.set_inodes_ever(getMdStat().inodes_ever());
  hbs.set_inodes_ever_deleted(getMdStat().inodes_deleted_ever());
  hbs.set_threads(osstat.threads);
  hbs.set_vsize_mb(osstat.vsize / 1024.0 / 1024.0);
  hbs.set_rss_mb(osstat.rss / 1024.0 / 1024.0);
}

/* -------------------------------------------------------------------------- */
bool
EosFuse::isRecursiveRm(fuse_req_t req, bool forced, bool notverbose)
/* -------------------------------------------------------------------------- */
{
#ifndef __APPLE__
  const struct fuse_ctx* ctx = fuse_req_ctx(req);
  ProcessSnapshot snapshot = fusexrdlogin::processCache->retrieve(ctx->pid,
			     ctx->uid, ctx->gid, false);

  if (snapshot->getProcessInfo().getRmInfo().isRm() &&
      snapshot->getProcessInfo().getRmInfo().isRecursive()) {
    bool result = true;

    if (forced) {
      // check if this is rm -rf style
      result = snapshot->getProcessInfo().getRmInfo().isForce();
    }

    if (notverbose) {
      result &= (!snapshot->getProcessInfo().getRmInfo().isVerbose());
    }

    return result;
  }

#endif
  return false;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::TrackMgm(const std::string& lasturl)
/* -------------------------------------------------------------------------- */
{
  static std::mutex lTrackMgmMutex;
  std::lock_guard<std::mutex> sequenzerMutex(lTrackMgmMutex);
  std::string currentmgm = lastMgmHostPort.get();
  XrdCl::URL lastUrl(lasturl);
  std::string newmgm = lastUrl.GetHostName();
  std::string sport;
  newmgm += ":";
  newmgm += eos::common::StringConversion::GetSizeString(sport,
	    (unsigned long long) lastUrl.GetPort());
  eos_static_debug("current-mgm:%s last-url:%s", currentmgm.c_str(),
		   newmgm.c_str());

  if (currentmgm != newmgm) {
    // for the first call currentmgm is an empty string, so we assume there is no failover needed
    if (currentmgm.length()) {
      // let's failover the ZMQ connection
      size_t p_pos = config.mqtargethost.rfind(":");
      std::string new_mqtargethost = config.mqtargethost;

      if ((p_pos != std::string::npos) && (p_pos > 6)) {
	new_mqtargethost.erase(6, p_pos - 6);
      } else {
	new_mqtargethost.erase(4);
      }

      lastMgmHostPort.set(newmgm);
      newmgm.erase(newmgm.find(":"));
      new_mqtargethost.insert(6, newmgm);
      // instruct a new ZMQ connection
      mds.connect(new_mqtargethost);
      eos_static_warning("reconnecting mqtarget=%s => mqtarget=%s",
			 config.mqtargethost.c_str(), new_mqtargethost.c_str());
    } else {
      // just store the first time we see the connected endpoint url
      lastMgmHostPort.set(newmgm);
    }
  }
}

/* -------------------------------------------------------------------------- */
std::string
EosFuse::Prefix(std::string path)
/* -------------------------------------------------------------------------- */
{
  std::string fullpath = Config().localmountdir;

  if (fullpath.back() == '/') {
    fullpath.pop_back();
  }

  return (fullpath + path);
}
