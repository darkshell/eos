/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016 CERN/Switzerland                                  *
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

//------------------------------------------------------------------------------
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief Class to serialize metadata to/from protobufs.
//------------------------------------------------------------------------------

#pragma once
#include "namespace/Namespace.hh"

namespace eos { namespace ns {
  class ContainerMdProto;
  class FileMdProto;
}}

EOSNSNAMESPACE_BEGIN

class Buffer;

class Serialization {
public:

  //----------------------------------------------------------------------------
  //! Deserialize a FileMD protobuf
  //----------------------------------------------------------------------------
  static void deserializeFile(const Buffer& buffer, eos::ns::FileMdProto &proto);
  static std::exception_ptr deserializeFileNoThrow(const Buffer& buffer, eos::ns::FileMdProto &proto);

  //----------------------------------------------------------------------------
  //! Deserialize a ContainerMD protobuf
  //----------------------------------------------------------------------------
  static void deserializeContainer(const Buffer& buffer, eos::ns::ContainerMdProto &proto);
  static std::exception_ptr deserializeContainerNoThrow(const Buffer& buffer, eos::ns::ContainerMdProto &proto);

};

EOSNSNAMESPACE_END