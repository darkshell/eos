// File: DebugCmd.hh
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

// #TOCK taken from IoCmd.hh
#pragma once
#include "mgm/Namespace.hh"
#include "proto/Debug.pb.h"
#include "mgm/proc/ProcCommand.hh"


EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class DebugCmd - class handling debug commands
//------------------------------------------------------------------------------
class DebugCmd: public IProcCommand
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client ProtocolBuffer request
  //! @param vid client virtual identity
  //----------------------------------------------------------------------------
  explicit DebugCmd(eos::console::RequestProto&& req,
                    eos::common::Mapping::VirtualIdentity& vid):
    IProcCommand(std::move(req), vid, false)
  {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~DebugCmd() = default;

  //----------------------------------------------------------------------------
  //! Method implementing the specific behaviour of the command executed by the
  //! asynchronous thread
  //----------------------------------------------------------------------------
  eos::console::ReplyProto ProcessRequest() noexcept override;

private:

  //----------------------------------------------------------------------------
  //! Execute get subcommand
  //!
  //! @param get get subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  int GetSubcmd(const eos::console::DebugProto_GetProto& get,
                eos::console::ReplyProto& reply);

  //----------------------------------------------------------------------------
  //! Execute set subcommand
  //!
  //! @param set set subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  int SetSubcmd(const eos::console::DebugProto_SetProto& set,
                eos::console::ReplyProto& reply);

};


EOSMGMNAMESPACE_END
