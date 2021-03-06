// ----------------------------------------------------------------------
// File: UserCredentialFactory.cc
// Author: Georgios Bitzes - CERN
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

#include "UserCredentialFactory.hh"
#include "Utils.hh"

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
UserCredentialFactory::UserCredentialFactory(const CredentialConfig &conf) :
  config(conf) {}

//------------------------------------------------------------------------------
// Parse a string, convert into SearchOrder
//------------------------------------------------------------------------------
SearchOrder UserCredentialFactory::parse(LogbookScope &scope,
  const std::string &str, const JailIdentifier &jail) {

  THROW("NYI");
}

//------------------------------------------------------------------------------
//! Append krb5 UserCredentials built from Environment, if KRB5CCNAME
//! is defined.
//------------------------------------------------------------------------------
void UserCredentialFactory::addKrb5FromEnv(const JailIdentifier &id,
  const Environment& env, uid_t uid, gid_t gid, SearchOrder &out)
{
  if(!config.use_user_krb5cc) {
    return;
  }

  std::string path = env.get("KRB5CCNAME");

  //--------------------------------------------------------------------------
  // Kerberos keyring?
  //--------------------------------------------------------------------------
  if(startswith(path, "KEYRING")) {
    out.emplace_back(UserCredentials::MakeKrk5(path, uid, gid));
    return;
  }

  //----------------------------------------------------------------------------
  // Drop FILE:, if exists
  //----------------------------------------------------------------------------
  const std::string prefix = "FILE:";
  if(startswith(path, prefix)) {
    path = path.substr(prefix.size());
  }

  if(path.empty()) {
    //--------------------------------------------------------------------------
    // Early exit, nothing to add to search order.
    //--------------------------------------------------------------------------
    return;
  }

  out.emplace_back(UserCredentials::MakeKrb5(id, path, uid, gid));
  return;
}

//------------------------------------------------------------------------------
// Append UserCredentials object built from X509_USER_PROXY
//------------------------------------------------------------------------------
void UserCredentialFactory::addx509FromEnv(const JailIdentifier &id,
  const Environment& env, uid_t uid, gid_t gid, SearchOrder &out)
{
  if(!config.use_user_gsiproxy) {
    return;
  }

  std::string path = env.get("X509_USER_PROXY");

  if (path.empty()) {
    //--------------------------------------------------------------------------
    // Early exit, nothing to add to search order.
    //--------------------------------------------------------------------------
    return;
  }

  out.emplace_back(UserCredentials::MakeX509(id, path, uid, gid));
  return;
}

//------------------------------------------------------------------------------
// Populate SearchOrder with entries given in environment variables.
//------------------------------------------------------------------------------
void UserCredentialFactory::addFromEnv(const JailIdentifier &id,
  const Environment& env, uid_t uid, gid_t gid, SearchOrder &searchOrder)
{
  //----------------------------------------------------------------------------
  // Using SSS? If so, add first.
  //----------------------------------------------------------------------------
  if(config.use_user_sss) {
    std::string endorsement = env.get("XrdSecsssENDORSEMENT");
    searchOrder.emplace_back(
      UserCredentials::MakeSSS(endorsement, uid, gid));
  }

  //----------------------------------------------------------------------------
  // Add krb5, x509 derived from environment variables
  //----------------------------------------------------------------------------
  addKrb5AndX509FromEnv(id, env, uid, gid, searchOrder);
}

//------------------------------------------------------------------------------
//! Append UserCredentials object built from krb5, and x509 env variables
//------------------------------------------------------------------------------
void UserCredentialFactory::addKrb5AndX509FromEnv(const JailIdentifier &id,
  const Environment &env, uid_t uid, gid_t gid, SearchOrder &out)
{
  if(config.tryKrb5First) {
    addKrb5FromEnv(id, env, uid, gid, out);
    addx509FromEnv(id, env, uid, gid, out);
  }
  else {
    addx509FromEnv(id, env, uid, gid, out);
    addKrb5FromEnv(id, env, uid, gid, out);
  }
}

//------------------------------------------------------------------------------
// Given a single entry of the search path, append any entries
// into the given SearchOrder object
//------------------------------------------------------------------------------
bool UserCredentialFactory::parseSingle(LogbookScope &scope, const std::string &str,
    const JailIdentifier &id, const Environment& env, uid_t uid, gid_t gid,
    SearchOrder &out)
{
  //----------------------------------------------------------------------------
  // Defaults?
  //----------------------------------------------------------------------------
  if(str == "defaults") {
    addFromEnv(id, env, uid, gid, out);
    return true;
  }

  //----------------------------------------------------------------------------
  // Cannot parse given string
  //----------------------------------------------------------------------------
  return false;
}
