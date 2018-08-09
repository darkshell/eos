// ----------------------------------------------------------------------
// File: GrpcNsInterface.hh
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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

#pragma once

#ifdef EOS_GRPC

/*----------------------------------------------------------------------------*/
#include "common/Mapping.hh"
#include "mgm/Namespace.hh"
#include "namespace/interface/IFileMD.hh"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/IView.hh"
/*----------------------------------------------------------------------------*/
#include "GrpcServer.hh"
#include "proto/Rpc.grpc.pb.h"
/*----------------------------------------------------------------------------*/
#include <grpc++/grpc++.h>

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

/**
 * @file   GrpcNsInterface.hh
 *
 * @brief  This class bridges namespace to gRPC requests
 *
 */


class GrpcNsInterface
{
public:

  static grpc::Status GetMD(eos::common::Mapping::VirtualIdentity_t& vid,
                            grpc::ServerWriter<eos::rpc::MDResponse>* writer,
                            const eos::rpc::MDRequest* request, bool check_perms = true);

  static grpc::Status StreamMD(eos::common::Mapping::VirtualIdentity_t& vid,
                               grpc::ServerWriter<eos::rpc::MDResponse>* writer,
                               const eos::rpc::MDRequest* request);

  static bool Access(eos::common::Mapping::VirtualIdentity_t& vid, int mode,
                     std::shared_ptr<eos::IContainerMD> cmd);

};

#endif


EOSMGMNAMESPACE_END

