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

#include "namespace/ns_quarkdb/accounting/FileSystemView.hh"
#include "namespace/ns_quarkdb/flusher/MetadataFlusher.hh"
#include "namespace/ns_quarkdb/persistency/RequestBuilder.hh"
#include "namespace/ns_quarkdb/BackendClient.hh"
#include "namespace/ns_quarkdb/Constants.hh"
#include "namespace/ns_quarkdb/FileMD.hh"
#include "common/StringTokenizer.hh"
#include "common/Logging.hh"
#include "qclient/QScanner.hh"
#include <iostream>
#include <folly/executors/IOThreadPoolExecutor.h>

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FileSystemView::FileSystemView():
  mExecutor(new folly::IOThreadPoolExecutor(8) ), pFlusher(nullptr), pQcl(nullptr)
{ }

//------------------------------------------------------------------------------
// Configure the container service
//------------------------------------------------------------------------------
void
FileSystemView::configure(const std::map<std::string, std::string>& config)
{
  std::string qdb_cluster;
  std::string qdb_flusher_id;
  const std::string key_cluster = "qdb_cluster";
  const std::string key_flusher = "qdb_flusher_md";

  if ((pQcl == nullptr) && (pFlusher == nullptr)) {
    if ((config.find(key_cluster) != config.end()) &&
        (config.find(key_flusher) != config.end())) {
      qdb_cluster = config.at(key_cluster);
      qdb_flusher_id = config.at(key_flusher);
    } else {
      eos::MDException e(EINVAL);
      e.getMessage() << __FUNCTION__  << " No " << key_cluster << " or "
                     << key_flusher << " configuration info provided";
      throw e;
    }

    qclient::Members qdb_members;

    if (!qdb_members.parse(qdb_cluster)) {
      eos::MDException e(EINVAL);
      e.getMessage() << __FUNCTION__ << " Failed to parse qdbcluster members: "
                     << qdb_cluster;
      throw e;
    }

    pQcl = BackendClient::getInstance(qdb_members);
    pFlusher = MetadataFlusherFactory::getInstance(qdb_flusher_id, qdb_members);
  }

  auto start = std::time(nullptr);
  loadFromBackend();
  auto end = std::time(nullptr);
  std::chrono::seconds duration(end - start);
  std::cerr << "FileSystemView loadingFromBackend duration: "
            << duration.count() << " seconds" << std::endl;

  mNoReplicas.reset(new FileSystemHandler(mExecutor.get(), pQcl, pFlusher, IsNoReplicaListTag() ));
}

//------------------------------------------------------------------------------
// Notify the me about changes in the main view
//------------------------------------------------------------------------------
void
FileSystemView::fileMDChanged(IFileMDChangeListener::Event* e)
{
  std::string key, val;
  FileMD* file = static_cast<FileMD*>(e->file);
  qclient::QSet fs_set;

  switch (e->action) {

  //----------------------------------------------------------------------------
  // New file has been created
  //----------------------------------------------------------------------------
  case IFileMDChangeListener::Created:
    if (!file->isLink()) {
      mNoReplicas->insert(file->getIdentifier());
    }

    break;

  //----------------------------------------------------------------------------
  // File has been deleted
  //----------------------------------------------------------------------------
  case IFileMDChangeListener::Deleted: {
    mNoReplicas->erase(file->getIdentifier());
    break;
  }

  //----------------------------------------------------------------------------
  // Add location
  //----------------------------------------------------------------------------
  case IFileMDChangeListener::LocationAdded: {
    FileSystemHandler *handler = initializeRegularFilelist(e->location);

    handler->insert(file->getIdentifier());
    mNoReplicas->erase(file->getIdentifier());
    break;
  }

  //----------------------------------------------------------------------------
  // Remove location.
  //
  // Perform destructive actions (ie erase) at the end.
  // This ensures that if we crash in the middle, we don't lose data, just
  // become inconsistent.
  //----------------------------------------------------------------------------
  case IFileMDChangeListener::LocationRemoved: {
    if (!file->getNumUnlinkedLocation() && !file->getNumLocation()) {
      mNoReplicas->insert(file->getIdentifier());
    }

    auto it = mUnlinkedFiles.find(e->location);
    if(it != mUnlinkedFiles.end()) {
      it->second.get()->erase(file->getIdentifier());
    }

    break;
  }

  //----------------------------------------------------------------------------
  // Unlink location.
  //
  // Perform destructive actions (ie erase) at the end.
  // This ensures that if we crash in the middle, we don't lose data, just
  // become inconsistent.
  //----------------------------------------------------------------------------
  case IFileMDChangeListener::LocationUnlinked: {
    FileSystemHandler* handler = initializeUnlinkedFilelist(e->location);
    handler->insert(e->file->getIdentifier());

    auto it = mFiles.find(e->location);

    if(it != mFiles.end()) {
      it->second.get()->erase(file->getIdentifier());
    }

    break;
  }

  default:
    break;
  }
}

//------------------------------------------------------------------------------
// Recheck the current file object and make any modifications necessary so
// that the information is consistent in the back-end KV store.
//------------------------------------------------------------------------------
bool
FileSystemView::fileMDCheck(IFileMD* file)
{
  std::string key;
  IFileMD::LocationVector replica_locs = file->getLocations();
  IFileMD::LocationVector unlink_locs = file->getUnlinkedLocations();
  bool has_no_replicas = replica_locs.empty() && unlink_locs.empty();
  std::string cursor {"0"};
  std::pair<std::string, std::vector<std::string>> reply;
  qclient::AsyncHandler ah;

  qclient::QSet no_replica_set(*pQcl, fsview::sNoReplicaPrefix);

  // If file has no replicas make sure it's accounted for
  if (has_no_replicas) {
    no_replica_set.sadd_async(file->getId(), &ah);
  } else {
    no_replica_set.srem_async(file->getId(), &ah);
  }

  // Make sure all active locations are accounted for
  qclient::QSet replica_set(*pQcl, "");

  for (IFileMD::location_t location : replica_locs) {
    replica_set.setKey(eos::RequestBuilder::keyFilesystemFiles(location));
    replica_set.sadd_async(file->getId(), &ah);
  }

  // Make sure all unlinked locations are accounted for.
  qclient::QSet unlink_set(*pQcl, "");

  for (IFileMD::location_t location : unlink_locs) {
    unlink_set.setKey(eos::RequestBuilder::keyFilesystemUnlinked(location));
    unlink_set.sadd_async(file->getId(), &ah);
  }

  // Make sure there's no other filesystems that erroneously contain this file.
  for (auto it = this->getFileSystemIterator(); it->valid(); it->next()) {
    IFileMD::location_t fsid = it->getElement();

    if (std::find(replica_locs.begin(), replica_locs.end(),
                  fsid) == replica_locs.end()) {
      replica_set.setKey(eos::RequestBuilder::keyFilesystemFiles(fsid));
      replica_set.srem_async(file->getId(), &ah);
    }

    if (std::find(unlink_locs.begin(), unlink_locs.end(),
                  fsid) == unlink_locs.end()) {
      unlink_set.setKey(eos::RequestBuilder::keyFilesystemUnlinked(fsid));
      unlink_set.srem_async(file->getId(), &ah);
    }
  }

  // Wait for all async responses
  return ah.Wait();
}

//------------------------------------------------------------------------------
// Get iterator object to run through all currently active filesystem IDs
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
    FileSystemView::getFileSystemIterator()
{
  return std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
         (new ListFileSystemIterator(mFiles));
}

//----------------------------------------------------------------------------
// Get iterator to list of files on a particular file system
//----------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getFileList(IFileMD::location_t location)
{
  auto iter = mFiles.find(location);

  if (iter == mFiles.end()) {
    return nullptr;
  }

  return iter->second.get()->getFileList();
}

//----------------------------------------------------------------------------
// Get an approximately random file residing within the given filesystem.
//----------------------------------------------------------------------------
bool FileSystemView::getApproximatelyRandomFileInFs(IFileMD::location_t location,
    IFileMD::id_t &retval) {

  auto iter = mFiles.find(location);

  if (iter == mFiles.end()) {
    return false;
  }

  return iter->second.get()->getApproximatelyRandomFile(retval);
}

//------------------------------------------------------------------------------
// Get iterator to list of unlinked files on a particular file system
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getUnlinkedFileList(IFileMD::location_t location)
{
  auto iter = mUnlinkedFiles.find(location);

  if(iter == mUnlinkedFiles.end()) {
    return nullptr;
  }

  return iter->second.get()->getFileList();
}

//------------------------------------------------------------------------------
// Get iterator to list of files without replicas
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getNoReplicasFileList()
{
  return mNoReplicas->getFileList();
}

//------------------------------------------------------------------------------
// Get number of files with no replicas
//------------------------------------------------------------------------------
uint64_t
FileSystemView::getNumNoReplicasFiles()
{
  return mNoReplicas->size();
}

//------------------------------------------------------------------------------
// Get number of files on the given file system
//------------------------------------------------------------------------------
uint64_t
FileSystemView::getNumFilesOnFs(IFileMD::location_t fs_id)
{
  auto iter = mFiles.find(fs_id);

  if(iter == mFiles.end()) {
    return 0ull;
  }

  return iter->second.get()->size();
}

//------------------------------------------------------------------------------
// Get number of unlinked files on the given file system
//------------------------------------------------------------------------------
uint64_t
FileSystemView::getNumUnlinkedFilesOnFs(IFileMD::location_t fs_id)
{
  auto iter = mUnlinkedFiles.find(fs_id);

  if(iter == mUnlinkedFiles.end()) {
    return 0ull;
  }

  return iter->second.get()->size();
}

//------------------------------------------------------------------------------
// Check if file system has file id
//------------------------------------------------------------------------------
bool
FileSystemView::hasFileId(IFileMD::id_t fid, IFileMD::location_t fs_id)
{
  auto iter = mFiles.find(fs_id);

  if(iter == mFiles.end()) {
    return false;
  }

  return iter->second.get()->hasFileId(fid);
}

//------------------------------------------------------------------------------
// Clear unlinked files for filesystem
//------------------------------------------------------------------------------
bool
FileSystemView::clearUnlinkedFileList(IFileMD::location_t location)
{
  auto it = mUnlinkedFiles.find(location);

  if (it == mUnlinkedFiles.end()) {
    return false;
  }

  it->second.get()->nuke();
  return true;
}

//----------------------------------------------------------------------------
// Parse an fs set key, returning its id and whether it points to "files" or
// "unlinked"
//----------------------------------------------------------------------------
bool parseFsId(const std::string& str, IFileMD::location_t& fsid,
               bool& unlinked)
{
  std::vector<std::string> parts =
    eos::common::StringTokenizer::split<std::vector<std::string>>(str, ':');

  if (parts.size() != 3) {
    return false;
  }

  if (parts[0] + ":" != fsview::sPrefix) {
    return false;
  }

  fsid = std::stoull(parts[1]);

  if (parts[2] == fsview::sFilesSuffix) {
    unlinked = false;
  } else if (parts[2] == fsview::sUnlinkedSuffix) {
    unlinked = true;
  } else {
    return false;
  }

  return true;
}

//----------------------------------------------------------------------------
// Get iterator object to run through all currently active filesystem IDs
//----------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
    FileSystemView::getQdbFileSystemIterator(const std::string& pattern)
{
  qclient::QScanner replicaSets(*pQcl, pattern);
  std::set<IFileMD::location_t> uniqueFilesytems;
  std::vector<std::string> results;

  while (replicaSets.next(results)) {
    for (std::string& rep : results) {
      // Extract fsid from key
      IFileMD::location_t fsid;
      bool unused;

      if (!parseFsId(rep, fsid, unused)) {
        eos_static_crit("Unable to parse key: %s", rep.c_str());
        continue;
      }

      uniqueFilesytems.insert(fsid);
    }
  }

  return std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
         (new QdbFileSystemIterator(std::move(uniqueFilesytems)));
}

//------------------------------------------------------------------------------
// Get iterator to list of files on a particular file system
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getQdbFileList(IFileMD::location_t location)
{
  std::string key = eos::RequestBuilder::keyFilesystemFiles(location);
  return std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
         (new QdbFileIterator(*pQcl, key));
}

//------------------------------------------------------------------------------
// Get iterator to list of unlinked files on a particular file system
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getQdbUnlinkedFileList(IFileMD::location_t location)
{
  std::string key = eos::RequestBuilder::keyFilesystemUnlinked(location);
  return std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
         (new QdbFileIterator(*pQcl, key));
}

//------------------------------------------------------------------------------
// Get iterator to list of files without replicas
//------------------------------------------------------------------------------
std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
    FileSystemView::getStreamingNoReplicasFileList()
{
  return std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
         (new QdbFileIterator(*pQcl, fsview::sNoReplicaPrefix));
}

//------------------------------------------------------------------------------
// Load view from backend
//------------------------------------------------------------------------------
void
FileSystemView::loadFromBackend()
{
  std::vector<std::string> patterns {
    fsview::sPrefix + "*:files",
    fsview::sPrefix + "*:unlinked" };

  for (const auto& pattern : patterns) {
    for (auto it = getQdbFileSystemIterator(pattern);
         (it && it->valid()); it->next()) {
      IFileMD::location_t fsid = it->getElement();

      if (pattern.find("unlinked") != std::string::npos) {
        initializeUnlinkedFilelist(fsid);
      } else {
        initializeRegularFilelist(fsid);
      }
    }
  }
}

//------------------------------------------------------------------------------
//! Initialize FileSystemHandler for given filesystem ID, if not already
//! initialized. Otherwise, do nothing.
//!
//! In any case, return pointer to the corresponding FileSystemHandler.
//!
//! @param fsid file system id
//------------------------------------------------------------------------------
FileSystemHandler* FileSystemView::initializeRegularFilelist(IFileMD::location_t fsid)
{
  auto iter = mFiles.find(fsid);

  if(iter != mFiles.end()) {
    // Found
    return iter->second.get();
  }

  mFiles[fsid].reset(new FileSystemHandler(fsid, mExecutor.get(), pQcl, pFlusher, false));
  return mFiles[fsid].get();
}

//------------------------------------------------------------------------------
//! Initialize unlinked FileSystemHandler for given filesystem ID,
//! if not already initialized. Otherwise, do nothing.
//!
//! In any case, return pointer to the corresponding FileSystemHandler.
//!
//! @param fsid file system id
//------------------------------------------------------------------------------
FileSystemHandler* FileSystemView::initializeUnlinkedFilelist(IFileMD::location_t fsid)
{
  auto iter = mUnlinkedFiles.find(fsid);

  if(iter != mUnlinkedFiles.end()) {
    // Found
    return iter->second.get();
  }

  mUnlinkedFiles[fsid].reset(new FileSystemHandler(fsid, mExecutor.get(), pQcl, pFlusher, true));
  return mUnlinkedFiles[fsid].get();
}

EOSNSNAMESPACE_END
