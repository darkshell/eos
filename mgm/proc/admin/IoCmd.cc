//------------------------------------------------------------------------------
// File: IoCmd.cc
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

#include "IoCmd.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Iostat.hh"

//#include "mgm/FsView.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behavior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
IoCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::IoProto io = mReqProto.io();
  eos::console::IoProto::SubcmdCase subcmd = io.subcmd_case();

  switch (subcmd) {
  case eos::console::IoProto::kStat:
    StatSubcmd(io.stat(), reply);
    break;

  case eos::console::IoProto::kEnable:
    EnableSubcmd(io.enable(), reply);
    break;

  case eos::console::IoProto::kDisable:
    DisableSubcmd(io.disable(), reply);
    break;

  case eos::console::IoProto::kReport:
    ReportSubcmd(io.report(), reply);
    break;

  case eos::console::IoProto::kNs:
    NsSubcmd(io.ns(), reply);
    break;

  default:
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

int IoCmd::StatSubcmd(const eos::console::IoProto_StatProto& stat,
                      eos::console::ReplyProto& reply)
{
  // If nothing is selected, we show the summary information
  if (!(stat.apps() | stat.domain() | stat.top() | stat.details())) {
    //stat.set_summary(true);
    eos_static_info("io stat");
    gOFS->IoStats->PrintOut(stdOut, true, stat.details(), stat.monitoring(),
                            stat.numerical(),
                            stat.top(), stat.domain(), stat.apps());
  } else {
    eos_static_info("io stat");
    gOFS->IoStats->PrintOut(stdOut, stat.summary(), stat.details(),
                            stat.monitoring(), stat.numerical(),
                            stat.top(), stat.domain(), stat.apps());
  }

  return SFS_OK; //TOCK is this needed?
}

int IoCmd::EnableSubcmd(const eos::console::IoProto_EnableProto& enable,
                        eos::console::ReplyProto& reply)
{
  if ((!enable.reports()) && (!enable.namespacex())) {
    if (enable.upd_address().length()) {
      if (gOFS->IoStats->AddUdpTarget(enable.upd_address().c_str())) {
        stdOut += ("success: enabled IO udp target " + enable.upd_address()).c_str();
      } else {
        stdErr += ("error: IO udp target was not configured " +
                   enable.upd_address()).c_str();
        retc = EINVAL;
      }
    } else {
      if (enable.popularity()) {
        gOFS->IoStats->Start(); // always enable collection otherwise we don't get anything for popularity reporting

        if (gOFS->IoStats->StartPopularity()) {
          stdOut += "success: enabled IO popularity collection";
        } else {
          stdErr += "error: IO popularity collection already enabled";
          retc = EINVAL;
        }
      } else {
        if (gOFS->IoStats->StartCollection()) {
          stdOut += "success: enabled IO report collection";
        } else {
          stdErr += "error: IO report collection already enabled";
          retc = EINVAL;
        }
      }
    }
  } else {
    if (enable.reports()) {
      if (gOFS->IoStats->StartReport()) {
        stdOut += "success: enabled IO report store";
      } else {
        stdErr += "error: IO report store already enabled";
        retc = EINVAL;
      }
    }

    if (enable.namespacex()) {
      if (gOFS->IoStats->StartReportNamespace()) {
        stdOut += "success: enabled IO report namespace";
      } else {
        stdErr += "error: IO report namespace already enabled";
        retc = EINVAL;
      }
    }
  }

  return SFS_OK;
}

int IoCmd::DisableSubcmd(const eos::console::IoProto_DisableProto& disable,
                         eos::console::ReplyProto& reply)
{
  if ((!disable.reports()) && (!disable.namespacex())) {
    if (disable.upd_address().length()) {
      if (gOFS->IoStats->RemoveUdpTarget(disable.upd_address().c_str())) {
        stdOut += ("success: disabled IO udp target " + disable.upd_address()).c_str();
      } else {
        stdErr += ("error: IO udp target was not configured " +
                   disable.upd_address()).c_str();
        retc = EINVAL;
      }
    } else {
      if (disable.popularity()) {
        if (gOFS->IoStats->StopPopularity()) {
          stdOut += "success: disabled IO popularity collection";
        } else {
          stdErr += "error: IO popularity collection already disabled";
          retc = EINVAL;
        }
      } else {
        if (gOFS->IoStats->StopCollection()) {
          stdOut += "success: disabled IO report collection";
        } else {
          stdErr += "error: IO report collection already disabled";
          retc = EINVAL;
        }
      }
    }
  } else {
    if (disable.reports()) {
      if (gOFS->IoStats->StopReport()) {
        stdOut += "success: disabled IO report store";
      } else {
        stdErr += "error: IO report store already enabled";
        retc = EINVAL;
      }
    }

    if (disable.namespacex()) {
      if (gOFS->IoStats->StopReportNamespace()) {
        stdOut += "success: disabled IO report namespace";
      } else {
        stdErr += "error: IO report namespace already disabled";
        retc = EINVAL;
      }
    }
  }

  return SFS_OK;
}

int IoCmd::ReportSubcmd(const eos::console::IoProto_ReportProto& report,
                        eos::console::ReplyProto& reply)
{
  if (mVid.uid == 0) {
    retc = Iostat::NamespaceReport(report.path().c_str(), stdOut, stdErr);
  }

  return retc;
}

int IoCmd::NsSubcmd(const eos::console::IoProto_NsProto& ns,
                    eos::console::ReplyProto& reply)
{
  std::string option;

  if (ns.monitoring()) {
    option += "-m";
  }

  if (ns.rank_by_byte()) {
    option += "-b";
  }

  if (ns.rank_by_access()) {
    option += "-n";
  }

  if (ns.last_week()) {
    option += "-w";
  }

  if (ns.hotfiles()) {
    option += "-f";
  }

  switch (ns.count()) {
  case eos::console::IoProto_NsProto::ONEHUNDRED:
    option += "-100";
    break;

  case eos::console::IoProto_NsProto::ONETHOUSAND:
    option += "-1000";
    break;

  case eos::console::IoProto_NsProto::TENTHOUSAND:
    option += "-10000";
    break;

  case eos::console::IoProto_NsProto::ALL:
    option += "-a";
    break;

  default : // NONE
    stdErr = "error: illegal parameter 'count'";
    retc = EINVAL;
    break;
  }

  eos_static_info("io ns");
  gOFS->IoStats->PrintNs(stdOut, option.c_str());
  reply.set_std_out(stdOut.c_str());
  reply.set_std_err(stdErr.c_str());
  reply.set_retc(retc);
  return SFS_OK;
}

EOSMGMNAMESPACE_END
