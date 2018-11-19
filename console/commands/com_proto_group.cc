//------------------------------------------------------------------------------
// File: com_proto_group.cc
// Author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your token) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/


#include "common/StringTokenizer.hh"
#include "common/Path.hh"
#include "console/ConsoleMain.hh"
#include "console/commands/ICmdHelper.hh"

//extern int com_group(char *); // #TOCK

int com_group_help();

//------------------------------------------------------------------------------
//! Class GroupHelper
//------------------------------------------------------------------------------
class GroupHelper : public ICmdHelper
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  GroupHelper()
  {
    mIsSilent = false;
    mHighlight = true;
  }
  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~GroupHelper() = default;

  //----------------------------------------------------------------------------
  //! Parse command line input
  //!
  //! @param arg input
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool ParseCommand(const char* arg) override;

};


bool GroupHelper::ParseCommand(const char* arg)
{
  eos::console::GroupProto* group = mReq.mutable_group();
  eos::common::StringTokenizer tokenizer(arg);
  tokenizer.GetLine();
  std::string token;

  if (!next_token(tokenizer, token)) {
    return false;
  }

  /* one of { ls, rm, set } */
  if (token == "ls") {
    eos::console::GroupProto_LsProto* ls = group->mutable_ls();

    while (next_token(tokenizer, token)) {
      if (token == "-s") {
        mIsSilent = true;
      } else if (token == "-g") {
        if (!next_token(tokenizer, token) || !tokenizer.IsUnsignedNumber(token)) {
          fprintf(stderr,
                  "Error: geodepth was not provided or it does not have the correct value: geodepth should be a positive integer\n");
          return false;
        }

        ls->set_outdepth(std::stoi(token));
      } else if (token == "-b" || token == "--brief") {
        ls->set_outhost(true);
        // } else if (token == "-m" || token == "-l" || token == "--io" || token == "--IO") {
      } else if (token == "-m") {
        ls->set_outformat(eos::console::GroupProto_LsProto::MONITORING);
      } else if (token == "-l") {
        ls->set_outformat(eos::console::GroupProto_LsProto::LONGER);
      } else if (token == "--io") {
        ls->set_outformat(eos::console::GroupProto_LsProto::IOGROUP);
      } else if (token == "--IO") {
        ls->set_outformat(eos::console::GroupProto_LsProto::IOFS);
      } else if (!(token.find("-") == 0)) { // begins with "-"
        ls->set_selection(token);
      } else {
        return false;
      }
    }
  } else if (token == "rm") {
    if (!next_token(tokenizer, token)) {
      return false;
    }

    eos::console::GroupProto_RmProto* rm = group->mutable_rm();
    rm->set_group(token);
  } else if (token == "set") {
    if (!next_token(tokenizer, token)) {
      return false;
    }

    eos::console::GroupProto_SetProto* set = group->mutable_set();
    set->set_group(token);

    if (!(next_token(tokenizer, token))) {
      return false;
    } else {
      if (token == "on") {
        set->set_group_state(true);
      } else if (token == "off") {
        set->set_group_state(false);
      } else {
        return false;
      }
    }
  } else { // no proper subcommand
    return false;
  }

  return true;
}


//------------------------------------------------------------------------------
// Group command entry point
//------------------------------------------------------------------------------
int com_protogroup(char* arg)
{
  if (wants_help(arg)) {
    com_group_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  GroupHelper group;

  if (!group.ParseCommand(arg)) {
    com_group_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  global_retc = group.Execute();
  return global_retc;
}

int com_group_help()
{
  std::ostringstream oss;
  oss
      << "usage: group ls [-s] [-g] [-b|--brief] [-m|-l|--io] [<groups>] : list groups"
      << std::endl
      << "\t <groups> : list <groups> only, where <groups> is a substring match and can be a comma seperated list"
      << std::endl
      << "\t       -s : silent mode" << std::endl
      << "\t       -g : geo output - aggregate group information along the instance geotree down to <depth>"
      << std::endl
      << "\t       -b : " << std::endl
      << "\t       -m : monitoring key=value output format" << std::endl
      << "\t       -l : long output - list also file systems after each group"
      << std::endl
      << "\t     --io : print IO statistics for the group" << std::endl
      << "\t     --IO : print IO statistics for each filesystem" << std::endl // #TOCK
      << std::endl
      << "usage: group rm <group-name> : remove group" << std::endl
      << std::endl
      << "usage: group set <group-name> on|off : activate/deactivate group"
      << std::endl
      << "\t  => when a group is (re-)enabled, the drain pull flag is recomputed for all filesystems within a group"
      << std::endl
      << "\t  => when a group is (re-)disabled, the drain pull flag is removed from all members in the group"
      << std::endl
      << std::endl;
  std::cerr << oss.str() << std::endl;
  global_retc = EINVAL;
  return (0);
}
