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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/convenience.hpp>
#include <fcntl.h>
#include <fstream>
#include <map>

#include "mongo/base/status.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/db.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/tools/mongodump_options.h"
#include "mongo/tools/tool.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;

class Dump : public Tool {
    class FilePtr : boost::noncopyable {
    public:
        /*implicit*/ FilePtr(FILE* f) : _f(f) {}
        ~FilePtr() { fclose(_f); }
        operator FILE*() { return _f; }
    private:
        FILE* _f;
    };
public:
    Dump() : Tool() { }

    virtual void printHelp(ostream& out) {
        printMongoDumpHelp(&out);
    }

    // This is a functor that writes a BSONObj to a file
    struct Writer {
        Writer(FILE* out, ProgressMeter* m) :_out(out), _m(m) {}

        void operator () (const BSONObj& obj) {
            size_t toWrite = obj.objsize();
            size_t written = 0;

            while (toWrite) {
                size_t ret = fwrite( obj.objdata()+written, 1, toWrite, _out );
                uassert(14035, errnoWithPrefix("couldn't write to file"), ret);
                toWrite -= ret;
                written += ret;
            }

            // if there's a progress bar, hit it
            if (_m) {
                _m->hit();
            }
        }

        FILE* _out;
        ProgressMeter* _m;
    };

    void doCollection( const string coll , Query q, FILE* out , ProgressMeter *m ) {
        int queryOptions = QueryOption_SlaveOk | QueryOption_NoCursorTimeout;
        if (startsWith(coll.c_str(), "local.oplog.") && q.obj.hasField("ts"))
            queryOptions |= QueryOption_OplogReplay;
        else if (mongoDumpGlobalParams.snapShotQuery) {
            q.snapshot();
        }
        
        DBClientBase& connBase = conn(true);
        Writer writer(out, m);

        // use low-latency "exhaust" mode if going over the network
        if (!_usingMongos && typeid(connBase) == typeid(DBClientConnection&)) {
            DBClientConnection& conn = static_cast<DBClientConnection&>(connBase);
            stdx::function<void(const BSONObj&)> castedWriter(writer); // needed for overload resolution
            conn.query( castedWriter, coll.c_str() , q , NULL, queryOptions | QueryOption_Exhaust);
        }
        else {
            //This branch should only be taken with DBDirectClient or mongos which doesn't support exhaust mode
            scoped_ptr<DBClientCursor> cursor(connBase.query( coll.c_str() , q , 0 , 0 , 0 , queryOptions ));
            while ( cursor->more() ) {
                writer(cursor->next());
            }
        }
    }

    void writeCollectionFile( const string coll , Query q, boost::filesystem::path outputFile ) {
        toolInfoLog() << "\t" << coll << " to " << outputFile.string() << std::endl;

        FilePtr f (fopen(outputFile.string().c_str(), "wb"));
        uassert(10262, errnoWithPrefix("couldn't open file"), f);

        ProgressMeter m(conn(true).count(coll.c_str(), BSONObj(), QueryOption_SlaveOk));
        m.setName("Collection File Writing Progress");
        m.setUnits("documents");

        doCollection(coll, q, f, &m);

        toolInfoLog() << "\t\t " << m.done()
                      << ((m.done() == 1) ? " document" : " documents")
                      << std::endl;
    }

    void writeMetadataFile( const string coll, boost::filesystem::path outputFile, 
                            map<string, BSONObj> options, multimap<string, BSONObj> indexes ) {
        toolInfoLog() << "\tMetadata for " << coll << " to " << outputFile.string() << std::endl;

        bool hasOptions = options.count(coll) > 0;
        bool hasIndexes = indexes.count(coll) > 0;

        BSONObjBuilder metadata;

        if (hasOptions) {
            metadata << "options" << options.find(coll)->second;
        }

        if (hasIndexes) {
            BSONArrayBuilder indexesOutput (metadata.subarrayStart("indexes"));

            // I'd kill for C++11 auto here...
            const pair<multimap<string, BSONObj>::iterator, multimap<string, BSONObj>::iterator>
                range = indexes.equal_range(coll);

            for (multimap<string, BSONObj>::iterator it=range.first; it!=range.second; ++it) {
                 indexesOutput << it->second;
            }

            indexesOutput.done();
        }

        ofstream file (outputFile.string().c_str());
        uassert(15933, "Couldn't open file: " + outputFile.string(), file.is_open());
        file << metadata.done().jsonString();
    }



    void writeCollectionStdout( const string coll ) {
        doCollection(coll, _query, stdout, NULL);
    }

    void go(const string& db,
            const string& coll,
            const Query& query,
            const boost::filesystem::path& outdir,
            const string& outFilename) {
        // Can only provide outFilename if db and coll are provided
        fassert(17368, outFilename.empty() || (!coll.empty() && !db.empty()));
        boost::filesystem::create_directories( outdir );

        map <string, BSONObj> collectionOptions;
        multimap <string, BSONObj> indexes;
        vector <string> collections;

        // Save indexes for database
        string ins = db + ".system.indexes";
        auto_ptr<DBClientCursor> cursor = conn( true ).query( ins.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->nextSafe();
            const string name = obj.getField( "ns" ).valuestr();
            indexes.insert( pair<string, BSONObj> (name, obj.getOwned()) );
        }

        string sns = db + ".system.namespaces";
        cursor = conn( true ).query( sns.c_str() , Query() , 0 , 0 , 0 , QueryOption_SlaveOk | QueryOption_NoCursorTimeout );
        while ( cursor->more() ) {
            BSONObj obj = cursor->nextSafe();
            const string name = obj.getField( "name" ).valuestr();
            if (obj.hasField("options")) {
                collectionOptions[name] = obj.getField("options").embeddedObject().getOwned();
            }

            // skip namespaces with $ in them only if we don't specify a collection to dump
            if (coll == "" && name.find(".$") != string::npos) {
                if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                    toolInfoLog() << "\tskipping collection: " << name << std::endl;
                }
                continue;
            }

            const string filename = name.substr( db.size() + 1 );

            //if a particular collections is specified, and it's not this one, skip it
            if (coll != "" && db + "." + coll != name && coll != name) {
                continue;
            }

            // raise error before writing collection with non-permitted filename chars in the name
            size_t hasBadChars = name.find_first_of("/\0");
            if (hasBadChars != string::npos){
                toolError() << "Cannot dump "  << name
                          << ". Collection has '/' or null in the collection name." << std::endl;
                continue;
            }

            if (nsToCollectionSubstring(name) == "system.indexes") {
              // Create system.indexes.bson for compatibility with pre 2.2 mongorestore
              const string filename = name.substr( db.size() + 1 );
              writeCollectionFile( name.c_str() , query, outdir / ( filename + ".bson" ) );
              // Don't dump indexes as *.metadata.json
              continue;
            }

            if (nsToCollectionSubstring(name) == "system.users" &&
                    !mongoDumpGlobalParams.dumpUsersAndRoles) {
                continue;
            }

            collections.push_back(name);
        }
        
        for (vector<string>::iterator it = collections.begin(); it != collections.end(); ++it) {
            string name = *it;
            const string filename = outFilename != "" ? outFilename : name.substr( db.size() + 1 );
            writeCollectionFile( name , query, outdir / ( filename + ".bson" ) );
            writeMetadataFile( name, outdir / (filename + ".metadata.json"), collectionOptions, indexes);
        }

    }

    int repair() {
        toolInfoLog() << "going to try and recover data from: " << toolGlobalParams.db << std::endl;
        return _repairByName(toolGlobalParams.db);
    }
    
    void _repairExtents(Collection* coll, Writer& writer) {
        scoped_ptr<RecordIterator> iter(coll->getRecordStore()->getIteratorForRepair());

        for (DiskLoc currLoc = iter->getNext(); !currLoc.isNull(); currLoc = iter->getNext()) {
            if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                toolInfoLog() << currLoc << std::endl;
            }

            BSONObj obj;
            try {
                obj = coll->docFor(currLoc);

                // If this is a corrupted object, just skip it, but do not abort the scan
                //
                if (!obj.valid()) {
                    continue;
                }

                if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                    toolInfoLog() << obj << std::endl;
                }

                writer(obj);
            }
            catch ( std::exception& e ) {
                toolError() << "found invalid document @ " << currLoc << " " << e.what() << std::endl;
                if ( ! obj.isEmpty() ) {
                    try {
                        BSONElement e = obj.firstElement();
                        stringstream ss;
                        ss << "first element: " << e;
                        toolError() << ss.str() << std::endl;
                    }
                    catch ( std::exception& ) {
                        toolError() << "unable to log invalid document @ " << currLoc << std::endl;
                    }
                }
            }
        }
    }

    /*
     * NOTE: The "outfile" parameter passed in should actually represent a directory, but it is
     * called "outfile" because we append the filename and use it as our output file.
     */
    void _repair(OperationContext* opCtx,
                 Database* db,
                 string ns,
                 boost::filesystem::path outfile) {
        Collection* collection = db->getCollection(opCtx, ns);
        toolInfoLog() << "nrecords: " << collection->numRecords()
                      << " datasize: " << collection->dataSize()
                      << std::endl;

        outfile /= ( ns.substr( ns.find( "." ) + 1 ) + ".bson" );
        toolInfoLog() << "writing to: " << outfile.string() << std::endl;

        FilePtr f (fopen(outfile.string().c_str(), "wb"));

        // init with double the docs count because we make two passes 
        ProgressMeter m( collection->numRecords() * 2 );
        m.setName("Repair Progress");
        m.setUnits("documents");

        Writer w( f , &m );

        try {
            _repairExtents(collection, w);
        }
        catch ( DBException& e ){
            toolError() << "Repair scan failed: " << e.toString() << std::endl;
        }

        toolInfoLog() << "\t\t " << m.done()
                      << ((m.done() == 1) ? " document" : " documents")
                      << std::endl;
    }
    
    int _repairByName(string dbname) {
        OperationContextImpl txn;
        Client::WriteContext cx(&txn, dbname);

        Database* db = dbHolder().get(dbname, storageGlobalParams.dbpath);

        list<string> namespaces;
        db->getDatabaseCatalogEntry()->getCollectionNamespaces( &namespaces );

        boost::filesystem::path root = mongoDumpGlobalParams.outputDirectory;
        root /= dbname;
        boost::filesystem::create_directories( root );

        for ( list<string>::iterator i=namespaces.begin(); i!=namespaces.end(); ++i ){
            LogIndentLevel lil;
            string ns = *i;

            if ( str::endsWith( ns , ".system.namespaces" ) )
                continue;
            
            if ( str::contains( ns , ".tmp.mr." ) )
                continue;
            
            if (toolGlobalParams.coll != "" &&
                !str::endsWith(ns, toolGlobalParams.coll)) {
                continue;
            }

            toolInfoLog() << "trying to recover: " << ns << std::endl;
            
            LogIndentLevel lil1;
            try {
                _repair( &txn, db , ns , root );
            }
            catch ( DBException& e ){
                toolError() << "ERROR recovering: " << ns << " " << e.toString() << std::endl;
            }
        }
   
        return 0;
    }

    int run() {
        if (mongoDumpGlobalParams.repair){
            return repair();
        }

        {
            if (mongoDumpGlobalParams.query.size()) {
                _query = fromjson(mongoDumpGlobalParams.query);
            }
        }

        if (mongoDumpGlobalParams.dumpUsersAndRoles) {
            uassertStatusOK(auth::getRemoteStoredAuthorizationVersion(&conn(true),
                                                                      &_serverAuthzVersion));
            uassert(17369,
                    mongoutils::str::stream() << "Backing up users and roles is only supported for "
                            "clusters with auth schema versions 1 or 3, found: " <<
                            _serverAuthzVersion,
                    _serverAuthzVersion == AuthorizationManager::schemaVersion24 ||
                    _serverAuthzVersion == AuthorizationManager::schemaVersion26Final);
        }

        string opLogName = "";
        unsigned long long opLogStart = 0;
        if (mongoDumpGlobalParams.useOplog) {

            BSONObj isMaster;
            conn("true").simpleCommand("admin", &isMaster, "isMaster");

            if (isMaster.hasField("hosts")) { // if connected to replica set member
                opLogName = "local.oplog.rs";
            }
            else {
                opLogName = "local.oplog.$main";
                if ( ! isMaster["ismaster"].trueValue() ) {
                    toolError() << "oplog mode is only supported on master or replica set member"
                              << std::endl;
                    return -1;
                }
            }

            BSONObj op = conn(true).findOne(opLogName, Query().sort("$natural", -1), 0, QueryOption_SlaveOk);
            if (op.isEmpty()) {
                toolError() << "No operations in oplog. Please ensure you are connecting to a "
                            << "master." << std::endl;
                return -1;
            }

            verify(op["ts"].type() == Timestamp);
            opLogStart = op["ts"]._numberLong();
        }

        // check if we're outputting to stdout
        if (mongoDumpGlobalParams.outputDirectory == "-") {
            if (toolGlobalParams.db != "" && toolGlobalParams.coll != "") {
                writeCollectionStdout(toolGlobalParams.db + "." + toolGlobalParams.coll);
                return 0;
            }
            else {
                toolError() << "You must specify database and collection to print to stdout"
                          << std::endl;
                return -1;
            }
        }

        _usingMongos = isMongos();

        boost::filesystem::path root(mongoDumpGlobalParams.outputDirectory);

        if (toolGlobalParams.db == "") {
            if (toolGlobalParams.coll != "") {
                toolError() << "--db must be specified with --collection" << std::endl;
                return -1;
            }

            toolInfoLog() << "all dbs" << std::endl;

            BSONObj res = conn( true ).findOne( "admin.$cmd" , BSON( "listDatabases" << 1 ) );
            if ( ! res["databases"].isABSONObj() ) {
                toolError() << "output of listDatabases isn't what we expected, no 'databases' "
                          << "field:\n" << res << std::endl;
                return -2;
            }
            BSONObj dbs = res["databases"].embeddedObjectUserCheck();
            set<string> keys;
            dbs.getFieldNames( keys );
            for ( set<string>::iterator i = keys.begin() ; i != keys.end() ; i++ ) {
                string key = *i;
                
                if ( ! dbs[key].isABSONObj() ) {
                    toolError() << "database field not an document key: " << key << " value: "
                              << dbs[key] << std::endl;
                    return -3;
                }

                BSONObj dbobj = dbs[key].embeddedObjectUserCheck();

                const char * dbName = dbobj.getField( "name" ).valuestr();
                if ( (string)dbName == "local" )
                    continue;

                boost::filesystem::path outdir = root / dbName;
                toolInfoLog() << "DATABASE: " << dbName << "\t to \t" << outdir.string()
                        << std::endl;
                go ( dbName , "", _query, outdir, "" );
            }
        }
        else {
            boost::filesystem::path outdir = root / toolGlobalParams.db;
            toolInfoLog() << "DATABASE: " << toolGlobalParams.db << "\t to \t" << outdir.string()
                    << std::endl;
            go(toolGlobalParams.db, toolGlobalParams.coll, _query, outdir, "");
            if (mongoDumpGlobalParams.dumpUsersAndRoles &&
                    _serverAuthzVersion == AuthorizationManager::schemaVersion26Final &&
                    toolGlobalParams.db != "admin") {
                toolInfoLog() << "Backing up user and role data for the " << toolGlobalParams.db <<
                        " database";
                Query query = Query(BSON("db" << toolGlobalParams.db));
                go("admin", "system.users", query, outdir, "$admin.system.users");
                go("admin", "system.roles", query, outdir, "$admin.system.roles");
            }
        }

        if (!opLogName.empty()) {
            BSONObjBuilder b;
            b.appendTimestamp("$gt", opLogStart);

            _query = BSON("ts" << b.obj());

            writeCollectionFile( opLogName , _query, root / "oplog.bson" );
        }

        return 0;
    }

    bool _usingMongos;
    int _serverAuthzVersion;
    BSONObj _query;
};

REGISTER_MONGO_TOOL(Dump);
