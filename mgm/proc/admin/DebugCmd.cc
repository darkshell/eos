// ----------------------------------------------------------------------
// File: proc/admin/Debug.cc
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

#include "DebugCmd.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Messaging.hh"

#include "mgm/FsView.hh"


EOSMGMNAMESPACE_BEGIN


//------------------------------------------------------------------------------
// Method implementing the specific behavior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
DebugCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::DebugProto debug = mReqProto.debug();
  eos::console::DebugProto::SubcmdCase subcmd = debug.subcmd_case();

  switch (subcmd) {
  case eos::console::DebugProto::kGet:
    GetSubcmd(debug.get(), reply);
    break;

  case eos::console::DebugProto::kSet:
    SetSubcmd(debug.set(), reply);
    break;

  default:
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

//----------------------------------------------------------------------------
// Execute get subcommand
//----------------------------------------------------------------------------
int DebugCmd::GetSubcmd(const eos::console::DebugProto_GetProto& get,
                        eos::console::ReplyProto& reply)
{
  /* get current loglevel of ?: "something" is missing */ // #TODO
  eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
  stdOut = (std::string("The current loglevel is: ") +
            g_logging.GetPriorityString(g_logging.GetLogMask())).c_str();  // #TOCK
  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK;
}

std::string rebuild_pOpaque(const eos::console::DebugProto_SetProto& set)
{
  std::string in = "mgm.cmd=debug";

  if (set.debuglevel().length()) {
    in += "&mgm.debuglevel=" + set.debuglevel();
  }

  if (set.nodename().length()) {
    in += "&mgm.nodename=" + set.nodename();
  }

  if (set.filter().length()) {
    in += "&mgm.filter=" + set.filter();
  }

  return in;
}

//----------------------------------------------------------------------------
// Execute set subcommand
//----------------------------------------------------------------------------
int DebugCmd::SetSubcmd(const eos::console::DebugProto_SetProto& set,
                        eos::console::ReplyProto& reply)
{
  if (mVid.uid != 0) {
    stdErr = "error: you have to take role 'root' to execute this command";
    retc = EPERM;
  } else {
    XrdMqMessage message("debug");
    // int envlen; //
    // std::string body = pOpaque->Env(envlen); //
    std::string body;
    int envlen;
    // filter out several *'s ...
    int nstars = 0;
    int npos = 0;

    while ((npos = set.nodename().find("*", npos)) != STR_NPOS) {
      npos++;
      nstars++;
    }

    if (nstars > 1) {
      stdErr = "error: debug level node can only contain one wildcard character (*) !";
      retc = EINVAL;
    } else {
      body = rebuild_pOpaque(set);
      envlen = body.length();
      message.SetBody(body.c_str());
      eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
      // always check debug level exists first
      int debugval = g_logging.GetPriorityByString(set.debuglevel().c_str());

      if (debugval < 0) {
        stdErr = ("error: debug level " + set.debuglevel() + " is not known!").c_str();
        retc = EINVAL;
      } else {
        if ((set.nodename() == "*") || (set.nodename() == "") ||
            (XrdOucString(set.nodename().c_str()) == gOFS->MgmOfsQueue)) {
          // this is for us!
          int debugval = g_logging.GetPriorityByString(set.debuglevel().c_str());
          g_logging.SetLogPriority(debugval);
          stdOut = ("success: debug level is now <" + set.debuglevel() + ">").c_str();
          eos_static_notice("setting debug level to <%s>", set.debuglevel());

          if (set.filter().length()) {
            g_logging.SetFilter(set.filter().c_str());
            stdOut += (" filter=" + set.filter()).c_str();
            eos_static_notice("setting message logid filter to <%s>", set.filter());
          }

          if (set.debuglevel() == "debug" &&
              ((g_logging.gAllowFilter.Num() &&
                g_logging.gAllowFilter.Find("SharedHash")) ||
               ((g_logging.gDenyFilter.Num() == 0) ||
                (g_logging.gDenyFilter.Find("SharedHash") == 0)))
             ) {
            gOFS->ObjectManager.SetDebug(true);
          } else {
            gOFS->ObjectManager.SetDebug(false);
          }
        }

        if (set.nodename() == "*") {
          std::string wildc_nodename;
          wildc_nodename = "/eos/*/fst";

          if (!Messaging::gMessageClient.SendMessage(message, wildc_nodename.c_str())) {
            stdErr = ("error: could not send debug level to nodes mgm.nodename=" +
                      wildc_nodename + "\n").c_str();
            retc = EINVAL;
          } else {
            stdOut = ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                      " on nodes mgm.nodename=" + wildc_nodename + "\n").c_str();
            eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                              set.debuglevel().c_str(), wildc_nodename.c_str());
          }

          wildc_nodename = "/eos/*/mgm";

          if (!Messaging::gMessageClient.SendMessage(message, wildc_nodename.c_str())) {
            stdErr += ("error: could not send debug level to nodes mgm.nodename=" +
                       wildc_nodename).c_str();
            retc = EINVAL;
          } else {
            stdOut += ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                       " on nodes mgm.nodename=" + wildc_nodename).c_str();
            eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                              set.debuglevel().c_str(), wildc_nodename.c_str());
          }
        } else {
          if (set.nodename() != "") {
            // send to the specified list
            if (!Messaging::gMessageClient.SendMessage(message, set.nodename().c_str())) {
              stdErr = ("error: could not send debug level to nodes mgm.nodename=" +
                        set.nodename()).c_str();
              retc = EINVAL;
            } else {
              stdOut = ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                        " on nodes mgm.nodename=" + set.nodename()).c_str();
              eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                                set.debuglevel().c_str(), set.nodename().c_str());
            }
          }
        }
      }
    }
  }

  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK;
}


EOSMGMNAMESPACE_END
