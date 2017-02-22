//
//  DbPointRecord.cpp
//  epanet-rtx
//
//  Open Water Analytics [wateranalytics.org]
//  See README.md and license.txt for more information
//


#include <iostream>
#include <sstream>
#include <set>
#include <vector>
#include <string>
#include <mutex>

#include "DbPointRecord.h"
#include "DbAdapter.h"

using namespace RTX;
using namespace std;
using boost::signals2::mutex;

#define _DB_MAX_CONNECT_TRY 5

/************ request type *******************/

DbPointRecord::request_t::request_t(string id, TimeRange r_range) : range(r_range), id(id) { }

bool DbPointRecord::request_t::contains(std::string id, time_t t) {
  if (this->range.start <= t 
      && t <= this->range.end 
      && RTX_STRINGS_ARE_EQUAL(id, this->id)) {
    return true;
  }
  return false;
}

void DbPointRecord::request_t::clear() {
  this->range = TimeRange();
  this->id = "";
}







/************ impl *******************/

DbPointRecord::DbPointRecord() : _last_request("",TimeRange()) {
  _adapter = NULL;
  errorMessage = "Not Connected";
  _readOnly = false;
  _filterType = OpcNoFilter;
  
  iterativeSearchMaxIterations = 8;
  iterativeSearchStride = 3*60*60;
    
  _errCB = [&](const std::string& msg)->void {
    this->errorMessage = msg;
  };
  
  this->setOpcFilterType(OpcPassThrough);
}

void DbPointRecord::setConnectionString(const std::string &str) {
  _adapter->setConnectionString(str);
}
string DbPointRecord::connectionString() {
  return _adapter->connectionString();
}


bool DbPointRecord::isConnected() {
  return _adapter->adapterConnected();
}

void DbPointRecord::dbConnect() throw(RtxException) {
  if (_adapter != NULL) {
    _adapter->doConnect();
  }
}

bool DbPointRecord::checkConnected() {
  int iTry = 0;
  while (!_adapter->adapterConnected() && ++iTry < _DB_MAX_CONNECT_TRY) {
    this->dbConnect();
  }
  return isConnected();
}

bool DbPointRecord::readonly() {
  if (_adapter->options().implementationReadonly) {
    return true;
  }
  else {
    return _readOnly;
  }
}

void DbPointRecord::setReadonly(bool readOnly) {
  if (_adapter->options().implementationReadonly) {
    _readOnly = false;
    return;
  }
  else {
    _readOnly = readOnly;
  }
}

bool DbPointRecord::registerAndGetIdentifierForSeriesWithUnits(string name, Units units) {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  
  if (name.length() == 0) {
    return false;
  }
  
  bool nameExists = false;
  bool unitsMatch = false;
  Units existingUnits = RTX_NO_UNITS;
  
  if (!checkConnected()) {
    return DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
  }
    
  auto match = this->identifiersAndUnits().doesHaveIdUnits(name,units);
  nameExists = match.first;
  unitsMatch = match.second;
  
  
  if (this->readonly()) {
    // handle a read-only database.
    if (nameExists && (unitsMatch || !_adapter->options().supportsUnitsColumn) ) {
      // everything is awesome. name matches, units match (or we don't support it and therefore don't care).
      // make a cache and return affirmative.
      DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
      return true;
    }
    else {
      // SPECIAL CASE FOR OLD RECORDS: we can update the units field if no units are specified.
      if (_adapter->options().canAssignUnits && existingUnits == RTX_NO_UNITS) {
        _adapter->assignUnitsToRecord(name, units);
        DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
        return true;
      }
      // names don't match (or units prevent us from using this record) and we can't write to this db. fail.
      return false;
    }
  }
  else {
    // not a readonly db.
    if (nameExists && !unitsMatch) {
      // two possibilities: the units actually don't match, or my units haven't ever been set.
      if (existingUnits == RTX_NO_UNITS) {
        if (_adapter->options().canAssignUnits) {
          // aha. update my units then.
          return _adapter->assignUnitsToRecord(name, units);
        }
      }
      else {
        // must remove the old record. units don't match for real.
        _adapter->removeRecord(name);
        return DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
      }
    }
    else if ( ( !nameExists || !unitsMatch ) && _adapter->insertIdentifierAndUnits(name, units) && DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units) ) {
      // this will either insert a new record name, or ignore because it's already there.
      return true;
    }
    else if (nameExists && unitsMatch) {
      return DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
    }
  }
  return false;
}


IdentifierUnitsList DbPointRecord::identifiersAndUnits() {
  
  time_t now = time(NULL);
  time_t stale = now - _lastIdRequest;
  _lastIdRequest = now;
  
  if (stale < 5 && !_identifiersAndUnitsCache.get()->empty()) {
    return _identifiersAndUnitsCache;
  }
  
  if (checkConnected()) {
    _identifiersAndUnitsCache = _adapter->idUnitsList();
  }
  return _identifiersAndUnitsCache;
}



void DbPointRecord::beginBulkOperation() {
  if (checkConnected()) {
    _adapter->beginTransaction();
  }
}

void DbPointRecord::endBulkOperation() {
  if (checkConnected()) {
    _adapter->endTransaction();
  }
}


Point DbPointRecord::point(const string& id, time_t time) {
  
  Point p = DB_PR_SUPER::point(id, time);
  
  if (!checkConnected()) {
    return p;
  }
  
  if (!p.isValid) {
    
    // see if we just asked the db for something in this range.
    // if so, and Super couldn't find it, then it's just not here.
    // todo -- check staleness
    
    if (_last_request.contains(id, time)) {
      return Point();
    }
    
    time_t margin = 60*60*12;
    time_t start = time - margin, end = time + margin;
    
    // do the request, and cache the request parameters.
    
    vector<Point> pVec = _adapter->selectRange(id, TimeRange(start, end));
    pVec = this->pointsWithOpcFilter(pVec);
    
    if (pVec.size() > 0) {
      _last_request = request_t(id, TimeRange(pVec.front().time, pVec.back().time));
    }
    else {
      _last_request = request_t(id,TimeRange());
    }
    
    
    vector<Point>::const_iterator pIt = pVec.begin();
    while (pIt != pVec.end()) {
      if (pIt->time == time) {
        p = *pIt;
      }
      else if (pIt->time > time) {
        break;
      }
      ++pIt;
    }
    // cache this latest result set
    DB_PR_SUPER::addPoints(id, pVec);
  }
  
  
  
  return p;
}


Point DbPointRecord::pointBefore(const string& id, time_t time) {
  
  // available in circular buffer?
  Point p = DB_PR_SUPER::pointBefore(id, time);
  if (p.isValid) {
    return p;
  }
  
  // if it's not buffered, but the last request covered this range, then there is no point here.
  if (_last_request.contains(id, time-1)) {
    return Point();
  }
  
  if (!checkConnected()) {
    return p;
  }
  
  // should i search iteratively?
  if (_adapter->options().searchIteratively) {
    p = this->searchPreviousIteratively(id, time);
    if (p.isValid) {
      return this->pointWithOpcFilter(p);
    }
  }
  
  
  // try a singly-bounded query
  if (_adapter->options().supportsSinglyBoundQuery) {
    p = _adapter->selectPrevious(id, time);
  }
  if (p.isValid) {
    return this->pointWithOpcFilter(p);
  }

  
  return p;
}


Point DbPointRecord::pointAfter(const string& id, time_t time) {
  // buffered?
  Point p = DB_PR_SUPER::pointAfter(id, time);
  if (p.isValid) {
    return p;
  }
  
  // last request covered this already?
  if (_last_request.contains(id, time + 1)) {
    return Point();
  }
  
  if (!checkConnected()) {
    return p;
  }
  
  if (_adapter->options().searchIteratively) {
    p = this->searchNextIteratively(id, time);
  }
  if (p.isValid) {
    return this->pointWithOpcFilter(p);
  }
  
  // singly bounded?
  if (_adapter->options().supportsSinglyBoundQuery) {
    p = _adapter->selectNext(id, time);
  }
  if (p.isValid) {
    return this->pointWithOpcFilter(p);
  }
  
  return p;
}


Point DbPointRecord::searchPreviousIteratively(const string& id, time_t time) {
  int lookBehindLimit = iterativeSearchMaxIterations;
  
  if (!checkConnected()) {
    return Point();
  }
  
  vector<Point> points;
  // iterative lookbehind is faster than unbounded lookup
  TimeRange r;
  r.start = time - iterativeSearchStride;
  r.end = time - 1;
  while (points.size() == 0 && lookBehindLimit > 0) {
    points = this->pointsInRange(id, r);
    r.end   -= iterativeSearchStride;
    r.start -= iterativeSearchStride;
    --lookBehindLimit;
  }
  if (points.size() > 0) {
    return points.back();
  }
  
  return Point();
}

Point DbPointRecord::searchNextIteratively(const string& id, time_t time) {
  int lookAheadLimit = iterativeSearchMaxIterations;
  
  if (!checkConnected()) {
    return Point();
  }
  
  vector<Point> points;
  // iterative lookbehind is faster than unbounded lookup
  TimeRange r;
  r.start = time + 1;
  r.end = time + iterativeSearchStride;
  while (points.size() == 0 && lookAheadLimit > 0) {
    points = this->pointsInRange(id, r);
    r.start += iterativeSearchStride;
    r.end += iterativeSearchStride;
    --lookAheadLimit;
  }
  if (points.size() > 0) {
    return points.front();
  }
  
  return Point();
}


std::vector<Point> DbPointRecord::pointsInRange(const string& id, TimeRange qrange) {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  
  // limit double-queries
  if (_last_request.range.containsRange(qrange) && _last_request.id == id) {
    return DB_PR_SUPER::pointsInRange(id, qrange);
  }
  
  if (!checkConnected()) {
    return DB_PR_SUPER::pointsInRange(id, qrange);
  }
  
  TimeRange range = DB_PR_SUPER::range(id);
  TimeRange::intersect_type intersect = range.intersection(qrange);

  // if the requested range is not in memcache, then fetch it.
  if ( intersect == TimeRange::intersect_other_internal ) {
    return DB_PR_SUPER::pointsInRange(id, qrange);
  }
  else {
    vector<Point> left, middle, right;
    TimeRange n_range;
    
    if (intersect == TimeRange::intersect_left) {
      // left-fill query
      n_range.start = qrange.start;
      n_range.end = range.start;
      middle = this->pointsWithOpcFilter(_adapter->selectRange(id, n_range));
      right = DB_PR_SUPER::pointsInRange(id, TimeRange(range.start, qrange.end));
    }
    else if (intersect == TimeRange::intersect_right) {
      // right-fill query
      n_range.start = range.end;
      n_range.end = qrange.end;
      left = DB_PR_SUPER::pointsInRange(id, TimeRange(qrange.start, range.end));
      middle = this->pointsWithOpcFilter(_adapter->selectRange(id, n_range));
    }
    else if (intersect == TimeRange::intersect_other_external){
      // query overlaps but extends on both sides
      TimeRange q_left, q_right;
      
      q_left.start = qrange.start;
      q_left.end = range.start;
      q_right.start = range.end;
      q_right.end = qrange.end;
      
      left = this->pointsWithOpcFilter(_adapter->selectRange(id, q_left));
      middle = DB_PR_SUPER::pointsInRange(id, range);
      right = this->pointsWithOpcFilter(_adapter->selectRange(id, q_right));
    }
    else {
      middle = this->pointsWithOpcFilter(_adapter->selectRange(id, qrange));
    }
    // db hit
    
    vector<Point> merged;
    merged.reserve(middle.size() + left.size() + right.size());
    merged.insert(merged.end(), left.begin(), left.end());
    merged.insert(merged.end(), middle.begin(), middle.end());
    merged.insert(merged.end(), right.begin(), right.end());

    set<time_t> addedTimes;
    vector<Point> deDuped;
    deDuped.reserve(merged.size());
    for(const Point& p : merged) {
      if (addedTimes.count(p.time) == 0) {
        addedTimes.insert(p.time);
        if (qrange.start <= p.time && p.time <= qrange.end) {
          deDuped.push_back(p);
        }
      }
    }
    
    _last_request = (deDuped.size() > 0) ? request_t(id, qrange) : request_t(id,TimeRange());
    DB_PR_SUPER::addPoints(id, deDuped);
    return deDuped;
  }
}


void DbPointRecord::addPoint(const string& id, Point point) {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  if (!this->readonly() && checkConnected()) {
    DB_PR_SUPER::addPoint(id, point);
    _adapter->insertSingle(id, point);
  }
}


void DbPointRecord::addPoints(const string& id, std::vector<Point> points) {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  if (!this->readonly() && checkConnected()) {
    DB_PR_SUPER::addPoints(id, points);
    _adapter->insertRange(id, points);
  }
}


void DbPointRecord::reset() {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  if (!this->readonly() && checkConnected()) {
    DB_PR_SUPER::reset();
    cerr << "deprecated. do not use" << endl;
    //this->truncate();
  }
}


void DbPointRecord::reset(const string& id) {
  std::lock_guard<std::mutex> lock(_db_pr_mtx);
  if (!this->readonly() && checkConnected()) {
    // deprecate?
    //cout << "Whoops - don't use this" << endl;
    DB_PR_SUPER::reset(id);
    _last_request.clear();
    //this->removeRecord(id);
    // wiped out the record completely, so re-initialize it.
    //this->registerAndGetIdentifier(id);
  }
}

void DbPointRecord::invalidate(const string &identifier) {
  if (!this->readonly() && checkConnected()) {
    _adapter->removeRecord(identifier);
    this->reset(identifier);
  }
}


#pragma mark - opc filter list

void DbPointRecord::setOpcFilterType(OpcFilterType type) {
  
  const map< OpcFilterType,function<Point(Point)> > opcFilters({
    { OpcPassThrough , 
      [&](Point p)->Point { 
        return p;
      } },
    { OpcWhiteList ,   
      [&](Point p)->Point { 
        if (this->opcFilterList().count(p.quality) > 0) {
          return Point(p.time, p.value, Point::opc_rtx_override, p.confidence);
        }
        else {
          return Point();
        }
      } },
    { OpcBlackList ,
      [&](Point p)->Point {
        if (this->opcFilterList().count(p.quality)) {
          return Point();
        }
        else {
          return Point(p.time, p.value, Point::opc_rtx_override, p.confidence);
        }
      }
    },
    { OpcCodesToValues ,
      [&](Point p)->Point {
        return Point(p.time, (double)p.quality, Point::opc_rtx_override, p.confidence);
      }
    },
    { OpcCodesToConfidence ,
      [&](Point p)->Point {
        return Point(p.time, p.value, Point::opc_rtx_override, (double)p.quality);
      }
    }
  });

  
  if (_filterType != type) {
    BufferPointRecord::reset(); // mem cache
    _filterType = type;
    _opcFilter = opcFilters.at(type);
  }
}

DbPointRecord::OpcFilterType DbPointRecord::opcFilterType() {
  return _filterType;
}

std::set<unsigned int> DbPointRecord::opcFilterList() {
  return _opcFilterCodes;
}

void DbPointRecord::clearOpcFilterList() {
  _opcFilterCodes.clear();
  BufferPointRecord::reset(); // mem cache
  this->dbConnect();
}

void DbPointRecord::addOpcFilterCode(unsigned int code) {
  _opcFilterCodes.insert(code);
  BufferPointRecord::reset(); // mem cache
  this->dbConnect();
}

void DbPointRecord::removeOpcFilterCode(unsigned int code) {
  if (_opcFilterCodes.count(code) > 0) {
    _opcFilterCodes.erase(code);
    BufferPointRecord::reset(); // mem cache
    this->dbConnect();
  }
}



vector<Point> DbPointRecord::pointsWithOpcFilter(std::vector<Point> points) {
  vector<Point> out;
  
  for(const Point& p : points) {
    Point outPoint = this->pointWithOpcFilter(p);
    if (outPoint.isValid) {
      out.push_back(outPoint);
    }
  }
  
  return out;
}


Point DbPointRecord::pointWithOpcFilter(Point p) {
  return _opcFilter(p);
}

