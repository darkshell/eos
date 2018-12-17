// ----------------------------------------------------------------------
// File: com_proto_debug.cc
// Author: Fabio Luchetti - CERN
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
#include "common/StringTokenizer.hh"
#include "common/Logging.hh"
#include "console/ConsoleMain.hh"
#include "console/commands/ICmdHelper.hh"

/*----------------------------------------------------------------------------*/


//extern int com_debug(char*);  // #TOCK

int com_debug_help();


//------------------------------------------------------------------------------
//! Class DebugHelper
//------------------------------------------------------------------------------
class DebugHelper : public ICmdHelper
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  DebugHelper()
  {  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~DebugHelper() = default;

  //----------------------------------------------------------------------------
  //! Parse command line input
  //!
  //! @param arg input
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool ParseCommand(const char* arg) override;
};


bool DebugHelper::ParseCommand(const char* arg)
{
  eos::console::DebugProto* debugp = mReq.mutable_debug();
  eos::common::StringTokenizer tokenizer(arg);
  tokenizer.GetLine();
  std::string token;

  // std::string level;
  // std::string nodequeue;
  // std::string filterlist;

  if (!next_token(tokenizer, token)) {
    return false;
  }

  // #TODO the whole if-tree should be simplified
  if ((token == "-h") || (token == "--help")) {
    return false;
  } else if (token == "getloglevel") {
    eos::console::DebugProto_GetProto* get = debugp->mutable_get();
    get->set_placeholder(true);
    //return true;
  } else {
    eos::console::DebugProto_SetProto* set = debugp->mutable_set();

    if (token == "this") {
      debug = !debug;
      fprintf(stdout, "info: toggling shell debugmode to debug=%d\n", debug);
      eos::common::Logging& g_logging = eos::common::Logging::GetInstance();

      if (debug) {
        g_logging.SetLogPriority(LOG_DEBUG);
      } else {
        g_logging.SetLogPriority(LOG_NOTICE);
      }

      //return true;
    } else {
      set->set_debuglevel(token);

      if (next_token(tokenizer, token)) {
        if (token == "--filter") {
          if (!next_token(tokenizer, token)) {
            return false;
          }

          set->set_filter(token);
          //return true;
        } else {
          set->set_nodename(token);
          next_token(tokenizer, token); // #TOCK if next_token do, else stop

          if (token == "--filter") {
            if (!next_token(tokenizer, token)) {
              return false;
            }

            set->set_filter(token);
          }
        }
      }
    }
  }

  return true;
}


/* Debug Level Setting */
int
com_protodebug(char* arg)
{
  if (wants_help(arg)) {
    com_debug_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  DebugHelper debug;

  if (!debug.ParseCommand(arg)) {
    com_debug_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  global_retc = debug.Execute();
  return global_retc;
}

int com_debug_help()
{
  std::ostringstream oss;
  oss
      << "usage: debug this|<level> [node-queue] [--filter <unitlist>]"
      << std::endl
      << "'[eos] debug ...' allows to modify the verbosity of the EOS log files in MGM and FST services."
      << std::endl
      << std::endl
      << "Options" << std::endl
      << "debug  this :" << std::endl
      << "                                                  toggle EOS shell debug mode"
      << std::endl
      << "debug  <level> [--filter <unitlist>] :" << std::endl
      << "                                                  set the MGM where the console is connected to into debug level <level>"
      << std::endl
      << "debug  <level> <node-queue> [--filter <unitlist>] :" << std::endl
      << "                                                  set the <node-queue> into debug level <level>. <node-queue> are internal EOS names e.g. '/eos/<hostname>:<port>/fst'"
      << std::endl
      << "     <unitlist> : a comma separated list of strings of software units which should be filtered out in the message log!"
      << std::endl
      << "                  The default filter list is: 'Process,AddQuota,Update,UpdateHint,UpdateQuotaStatus,SetConfigValue,Deletion,GetQuota,PrintOut,RegisterNode,SharedHash,listenFsChange,"
      << std::endl
      << "                  placeNewReplicas,placeNewReplicasOneGroup,accessReplicas,accessReplicasOneGroup,accessHeadReplicaMultipleGroup,updateTreeInfo,updateAtomicPenalties,updateFastStructures,work'."
      << std::endl
      << std::endl
      << "The allowed debug levels are: debug info warning notice err crit alert emerg"
      << std::endl
      << std::endl
      << "Examples:" << std::endl
      << "  debug info *                         set MGM & all FSTs into debug mode 'info'"
      << std::endl
      << std::endl
      << "  debug err /eos/*/fst                 set all FSTs into debug mode 'info'"
      << std::endl
      << std::endl
      << "  debug crit /eos/*/mgm                set MGM into debug mode 'crit'" <<
      std::endl
      << std::endl
      << "  debug debug --filter MgmOfsMessage   set MGM into debug mode 'debug' and filter only messages coming from unit 'MgmOfsMessage'."
      << std::endl
      << std::endl;
  std::cerr << oss.str() << std::endl;
  global_retc = EINVAL;
  return (0);
}
