/* @file rs_initiate.cpp
   */

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/heartbeat.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"  // replSettings
#include "mongo/db/repl/repl_start.h"  // parseReplsetCmdLine
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/util/mmap.h"
#include "mongo/util/mongoutils/str.h"

using namespace bson;
using namespace mongoutils;

namespace mongo {
namespace repl {

    /* called on a reconfig AND on initiate
       throws
       @param initial true when initiating
    */
    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, BSONObjBuilder& result, bool initial) {
        int failures = 0, allVotes = 0, allowableFailures = 0;
        int me = 0;
        stringstream selfs;
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            if( i->h.isSelf() ) {
                me++;
                if( me > 1 )
                    selfs << ',';
                selfs << i->h.toString();
                if( !i->potentiallyHot() ) {
                    uasserted(13420, "initiation and reconfiguration of a replica set must be sent to a node that can become primary");
                }
            }
            allVotes += i->votes;
        }
        allowableFailures = allVotes - (allVotes/2 + 1);

        uassert(13278, "bad config: isSelf is true for multiple hosts: " + selfs.str(), me <= 1); // dups?
        if( me != 1 ) {
            stringstream ss;
            ss << "can't find self in the replset config";
            if (!serverGlobalParams.isDefaultPort()) ss << " my port: " << serverGlobalParams.port;
            if( me != 0 ) ss << " found: " << me;
            uasserted(13279, ss.str());
        }

        vector<string> down;
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            // we know we're up
            if (i->h.isSelf()) {
                continue;
            }

            BSONObj res;
            {
                bool ok = false;
                try {
                    int theirVersion = -1000;
                    ok = requestHeartbeat(cfg._id, "", i->h.toString(), res, -1, theirVersion, initial/*check if empty*/);
                    if( theirVersion >= cfg.version ) {
                        stringstream ss;
                        ss << "replSet member " << i->h.toString() << " has too new a config version (" << theirVersion << ") to reconfigure";
                        uasserted(13259, ss.str());
                    }
                }
                catch(DBException& e) {
                    log() << "replSet cmufcc requestHeartbeat " << i->h.toString() << " : " << e.toString() << rsLog;
                }
                catch(...) {
                    log() << "replSet cmufcc error exception in requestHeartbeat?" << rsLog;
                }
                if( res.getBoolField("mismatch") )
                    uasserted(13145, "set name does not match the set name host " + i->h.toString() + " expects");
                if( *res.getStringField("set") ) {
                    if( cfg.version <= 1 ) {
                        // this was to be initiation, no one should be initiated already.
                        uasserted(13256, "member " + i->h.toString() + " is already initiated");
                    }
                    else {
                        // Assure no one has a newer config.
                        if( res["v"].Int() >= cfg.version ) {
                            uasserted(13341, "member " + i->h.toString() + " has a config version >= to the new cfg version; cannot change config");
                        }
                    }
                }
                if( !ok && !res["rs"].trueValue() ) {
                    down.push_back(i->h.toString());

                    if( !res.isEmpty() ) {
                        /* strange.  got a response, but not "ok". log it. */
                        log() << "replSet warning " << i->h.toString() << " replied: " << res.toString() << rsLog;
                    }

                    bool allowFailure = false;
                    failures += i->votes;
                    if( !initial && failures <= allowableFailures ) {
                        const Member* m = theReplSet->findById( i->_id );
                        if( m ) {
                            verify( m->h().toString() == i->h.toString() );
                        }
                        // it's okay if the down member isn't part of the config,
                        // we might be adding a new member that isn't up yet
                        allowFailure = true;
                    }

                    if( !allowFailure ) {
                        string msg = string("need all members up to initiate, not ok : ") + i->h.toString();
                        if( !initial )
                            msg = string("need most members up to reconfigure, not ok : ") + i->h.toString();
                        uasserted(13144, msg);
                    }
                }
            }
            if( initial ) {
                bool hasData = res["hasData"].Bool();
                uassert(13311, "member " + i->h.toString() + " has data already, cannot initiate set.  All members except initiator must be empty.",
                        !hasData || i->h.isSelf());
            }
        }
        if (down.size() > 0) {
            result.append("down", down);
        }
    }

    class CmdReplSetInitiate : public ReplSetCommand {
    public:
        virtual bool isWriteCommandForConfigServer() const { return false; }
        CmdReplSetInitiate() : ReplSetCommand("replSetInitiate") { }
        virtual void help(stringstream& h) const {
            h << "Initiate/christen a replica set.";
            h << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetConfigure);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "replSet replSetInitiate admin command received from client" << rsLog;

            if( !replSet ) {
                errmsg = "server is not running with --replSet";
                return false;
            }
            if( theReplSet ) {
                errmsg = "already initialized";
                result.append("info", "try querying " + rsConfigNs + " to see current configuration");
                return false;
            }

            {
                // just make sure we can get a write lock before doing anything else.  we'll reacquire one
                // later.  of course it could be stuck then, but this check lowers the risk if weird things
                // are up.
                time_t t = time(0);
                Lock::GlobalWrite lk(txn->lockState());
                if( time(0)-t > 10 ) {
                    errmsg = "took a long time to get write lock, so not initiating.  Initiate when server less busy?";
                    return false;
                }

                /* check that we don't already have an oplog.  that could cause issues.
                   it is ok if the initiating member has *other* data than that.
                   */
                BSONObj o;
                if( Helpers::getFirst(txn, rsoplog, o) ) {
                    errmsg = rsoplog + string(" is not empty on the initiating member.  cannot initiate.");
                    return false;
                }
            }

            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                errmsg = "server already in BADCONFIG state (check logs); not initiating";
                result.append("info", ReplSet::startupStatusMsg.get());
                return false;
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = "all members and seeds must be reachable to initiate set";
                result.append("info", replSettings.replSet);
                return false;
            }

            BSONObj configObj;

            if( cmdObj["replSetInitiate"].type() != Object ) {
                result.append("info2", "no configuration explicitly specified -- making one");
                log() << "replSet info initiate : no configuration specified.  Using a default configuration for the set" << rsLog;

                string name;
                vector<HostAndPort> seeds;
                set<HostAndPort> seedSet;
                parseReplsetCmdLine(replSettings.replSet, name, seeds, seedSet); // may throw...

                bob b;
                b.append("_id", name);
                bob members;
                members.append("0", BSON( "_id" << 0 << "host" << HostAndPort::me().toString() ));
                result.append("me", HostAndPort::me().toString());
                for( unsigned i = 0; i < seeds.size(); i++ )
                    members.append(bob::numStr(i+1), BSON( "_id" << i+1 << "host" << seeds[i].toString()));
                b.appendArray("members", members.obj());
                configObj = b.obj();
                log() << "replSet created this configuration for initiation : " << configObj.toString() << rsLog;
            }
            else {
                configObj = cmdObj["replSetInitiate"].Obj();
            }

            bool parsed = false;
            try {
                scoped_ptr<ReplSetConfig> newConfig(ReplSetConfig::make(configObj));
                parsed = true;

                if( newConfig->version > 1 ) {
                    errmsg = "can't initiate with a version number greater than 1";
                    return false;
                }

                log() << "replSet replSetInitiate config object parses ok, " <<
                        newConfig->members.size() << " members specified" << rsLog;

                checkMembersUpForConfigChange(*newConfig, result, true);

                log() << "replSet replSetInitiate all members seem up" << rsLog;

                createOplog();

                Lock::GlobalWrite lk(txn->lockState());
                bo comment = BSON( "msg" << "initiating set");
                newConfig->saveConfigLocally(comment);
                log() << "replSet replSetInitiate config now saved locally.  "
                    "Should come online in about a minute." << rsLog;
                result.append("info", "Config now saved locally.  Should come online "
                              "in about a minute.");
                ReplSet::startupStatus = ReplSet::SOON;
                ReplSet::startupStatusMsg.set("Received replSetInitiate - "
                                              "should come online shortly.");
            }
            catch( DBException& e ) {
                log() << "replSet replSetInitiate exception: " << e.what() << rsLog;
                if( !parsed )
                    errmsg = string("couldn't parse cfg object ") + e.what();
                else
                    errmsg = string("couldn't initiate : ") + e.what();
                return false;
            }
            catch( string& e2 ) {
                log() << e2 << rsLog;
                errmsg = e2;
                return false;
            }

            return true;
        }
    } cmdReplSetInitiate;

} // namespace repl
} // namespace mongo
