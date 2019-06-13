// Copyright (c) 2019 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#define _USE_MATH_DEFINES   // to avoid undefined error (bug in Visual Studio 2015 and 2017)
#include <cmath>

#include "carla/Logging.h"
#include "carla/nav/Navigation.h"

#include <iostream>
#include <iterator>
#include <fstream>

namespace carla {
namespace nav {

  static const int MAX_POLYS = 256;
  static const int MAX_AGENTS = 500;
  static const float AGENT_RADIUS = 0.3f;

  // return a random float
  float frand() {
    return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
  }

  Navigation::Navigation() {
  }

  Navigation::~Navigation() {
    dtFreeCrowd(_crowd);
    dtFreeNavMeshQuery(_navQuery);
    dtFreeNavMesh(_navMesh);
  }

  // load navigation data
  bool Navigation::Load(const std::string filename) {
    std::ifstream f;
    std::istream_iterator<uint8_t> start(f), end;

    // read the whole file
    f.open(filename, std::ios::binary);
    if (!f.is_open())
      return false;
    std::vector<uint8_t> content(start, end);
    f.close();

    // parse the content
    return Load(content);
  }

  // load navigation data from memory
  bool Navigation::Load(const std::vector<uint8_t> content) {
    const int NAVMESHSET_MAGIC = 'M'<<24 | 'S'<<16 | 'E'<<8 | 'T'; //'MSET';
    const int NAVMESHSET_VERSION = 1;
    #pragma pack(push, 1)
    struct NavMeshSetHeader {
      int magic;
      int version;
      int numTiles;
      dtNavMeshParams params;
    } header;
    struct NavMeshTileHeader {
      dtTileRef tileRef;
      int dataSize;
    };
    #pragma pack(pop)

    // read the file header
    unsigned long pos = 0;
    memcpy(&header, &content[pos], sizeof(header));
    pos += sizeof(header);

    // check file magic and version
    if (header.magic != NAVMESHSET_MAGIC || header.version != NAVMESHSET_VERSION) {
      return false;
    }

    // allocate object
    dtNavMesh* mesh = dtAllocNavMesh();
    // set number of tiles and origin
    dtStatus status = mesh->init(&header.params);
    if (dtStatusFailed(status)) {
      return false;
    }

    // read the tiles data
    for (int i = 0; i < header.numTiles; ++i) {
      NavMeshTileHeader tileHeader;

      // read the tile header
      memcpy(&tileHeader, &content[pos], sizeof(tileHeader));
      pos += sizeof(tileHeader);
      if (pos >= content.size()) {
        dtFreeNavMesh(mesh);
        return false;
      }

      // check for valid tile
      if (!tileHeader.tileRef || !tileHeader.dataSize)
        break;

      // allocate the buffer
      char* data = static_cast<char*>(dtAlloc(static_cast<size_t>(tileHeader.dataSize), DT_ALLOC_PERM));
      if (!data) break;

      // read the tile
      memset(data, 0, static_cast<size_t>(tileHeader.dataSize));
      memcpy(data, &content[pos], static_cast<size_t>(tileHeader.dataSize));
      pos += static_cast<unsigned long>(tileHeader.dataSize);
      if (pos > content.size()) {
        dtFree(data);
        dtFreeNavMesh(mesh);
        return false;
      }

      // add the tile data
      mesh->addTile(reinterpret_cast<unsigned char*>(data), tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, 0);
    }

    // exchange
    dtFreeNavMesh(_navMesh);
    _navMesh = mesh;

    // prepare the query object
    dtFreeNavMeshQuery(_navQuery);
    _navQuery = dtAllocNavMeshQuery();
    _navQuery->init(_navMesh, 2048);

    // create and init the crowd manager
    CreateCrowd();

    // copy
    _binaryMesh = content;

    return true;
  }

  void Navigation::CreateCrowd(void) {

    if (_crowd != nullptr)
      return;

    // create and init
    _crowd = dtAllocCrowd();
    _crowd->init(MAX_AGENTS, AGENT_RADIUS, _navMesh);

    // make polygons with 'disabled' flag invalid
    _crowd->getEditableFilter(0)->setExcludeFlags(SAMPLE_POLYFLAGS_DISABLED);

    // Setup local avoidance params to different qualities.
    dtObstacleAvoidanceParams params;
    // Use mostly default settings, copy from dtCrowd.
    memcpy(&params, _crowd->getObstacleAvoidanceParams(0), sizeof(dtObstacleAvoidanceParams));

    // Low (11)
    params.velBias = 0.5f;
    params.adaptiveDivs = 5;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 1;
    _crowd->setObstacleAvoidanceParams(0, &params);

    // Medium (22)
    params.velBias = 0.5f;
    params.adaptiveDivs = 5;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 2;
    _crowd->setObstacleAvoidanceParams(1, &params);

    // Good (45)
    params.velBias = 0.5f;
    params.adaptiveDivs = 7;
    params.adaptiveRings = 2;
    params.adaptiveDepth = 3;
    _crowd->setObstacleAvoidanceParams(2, &params);

    // High (66)
    params.velBias = 0.5f;
    params.adaptiveDivs = 7;
    params.adaptiveRings = 3;
    params.adaptiveDepth = 3;

    _crowd->setObstacleAvoidanceParams(3, &params);
  }

  // return the path points to go from one position to another
  bool Navigation::GetPath(const carla::geom::Location from, const carla::geom::Location to, dtQueryFilter* filter, std::vector<carla::geom::Location> &path) {
    // path found
    float m_straightPath[MAX_POLYS*3];
    unsigned char m_straightPathFlags[MAX_POLYS];
    dtPolyRef m_straightPathPolys[MAX_POLYS];
    int m_nstraightPath;
    int m_straightPathOptions = 0;
    // polys in path
    dtPolyRef m_polys[MAX_POLYS];
    int m_npolys;

    // check to load the binary _navMesh from server
    if (_binaryMesh.size() == 0) {
      return false;
    }

    // point extension
    float m_polyPickExt[3];
    m_polyPickExt[0] = 2;
    m_polyPickExt[1] = 4;
    m_polyPickExt[2] = 2;

    // filter
    dtQueryFilter filter2;
    if (filter == nullptr) {
      filter2.setIncludeFlags(SAMPLE_POLYFLAGS_ALL ^ SAMPLE_POLYFLAGS_DISABLED);
      filter2.setExcludeFlags(0);
      filter = &filter2;
    }

  	// set the points
    dtPolyRef m_startRef = 0;
    dtPolyRef m_endRef = 0;
    float m_spos[3] = { from.x, from.z, from.y };
	  float m_epos[3] = { to.x, to.z, to.y };
    _navQuery->findNearestPoly(m_spos, m_polyPickExt, filter, &m_startRef, 0);
    _navQuery->findNearestPoly(m_epos, m_polyPickExt, filter, &m_endRef, 0);
    if (!m_startRef || !m_endRef) {
      return false;
    }

    // get the path of nodes
	  _navQuery->findPath(m_startRef, m_endRef, m_spos, m_epos, filter, m_polys, &m_npolys, MAX_POLYS);

    // get the path of points
    m_nstraightPath = 0;
    if (m_npolys == 0) {
      return false;
    }

    // in case of partial path, make sure the end point is clamped to the last polygon
    float epos[3];
    dtVcopy(epos, m_epos);
    if (m_polys[m_npolys-1] != m_endRef)
      _navQuery->closestPointOnPoly(m_polys[m_npolys-1], m_epos, epos, 0);

    // get the points
    _navQuery->findStraightPath(m_spos, epos, m_polys, m_npolys,
                                 m_straightPath, m_straightPathFlags,
                                 m_straightPathPolys, &m_nstraightPath, MAX_POLYS, m_straightPathOptions);

    // copy the path to the output buffer
    path.clear();
    path.reserve(static_cast<unsigned long>(m_nstraightPath));
    for (int i=0; i<m_nstraightPath*3; i+=3) {
      // export for Unreal axis (x, z, y)
      path.emplace_back(m_straightPath[i], m_straightPath[i+2], m_straightPath[i+1]);
    }

    return true;
  }

  // create a new walker in crowd
  bool Navigation::AddWalker(ActorId id, carla::geom::Location from, float base_offset) {
    dtCrowdAgentParams params;

    if (_crowd == nullptr) {
      return false;
    }

    // set parameters
    memset(&params, 0, sizeof(params));
    params.radius = AGENT_RADIUS;
    params.height = base_offset * 2.0f;
    params.maxAcceleration = 8.0f;
    params.maxSpeed = 1.47f;
    params.collisionQueryRange = params.radius * 12.0f;
    params.pathOptimizationRange = params.radius * 30.0f;

    // flags
    params.updateFlags = 0;
    params.updateFlags |= DT_CROWD_ANTICIPATE_TURNS;
    params.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
    params.updateFlags |= DT_CROWD_OPTIMIZE_TOPO;
    params.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
    params.updateFlags |= DT_CROWD_SEPARATION;
    params.obstacleAvoidanceType = 3;
    params.separationWeight = 0.5f;


    // set from Unreal coordinates (and adjust center of walker, from middle to bottom)
    float PointFrom[3] = { from.x, from.z, from.y };
    // add walker
    int index = _crowd->addAgent(PointFrom, &params);
    if (index == -1) {
      return false;
    }

    // save the id
    _mappedId[id] = index;

    // save the base offset of this walker
    _baseHeight[id] = base_offset;

    yaw_walkers[id] = 0.0f;

    return true;
  }

  // set a new target point to go
  bool Navigation::SetWalkerTarget(ActorId id, carla::geom::Location to) {
    // get the internal index
    auto it = _mappedId.find(id);
    if (it == _mappedId.end())
      return false;

    return SetWalkerTargetIndex(it->second, to);
  }

  // set a new target point to go
  bool Navigation::SetWalkerTargetIndex(int index, carla::geom::Location to) {
    if (index == -1)
      return false;

    // set target position
    float pointTo[3] = { to.x, to.z, to.y };
    float nearest[3];
    const dtQueryFilter *filter = _crowd->getFilter(0);
    dtPolyRef targetRef;
    _navQuery->findNearestPoly(pointTo, _crowd->getQueryHalfExtents(), filter, &targetRef, nearest);
    if (!targetRef)
      return false;

    return _crowd->requestMoveTarget(index, targetRef, pointTo);
  }

  // update all walkers in crowd
  void Navigation::UpdateCrowd(const client::detail::EpisodeState &state) {

    if (!_navMesh || !_crowd) {
      return;
    }

    // force single thread running this
    std::lock_guard<std::mutex> lock(_mutex);

    // update all
    _delta_seconds = state.GetTimestamp().delta_seconds;
    _crowd->update(static_cast<float>(_delta_seconds), nullptr);

    // check if walker has finished
    for (int i = 0; i<_crowd->getAgentCount(); ++i)
    {
      const dtCrowdAgent* ag = _crowd->getAgent(i);
      if (!ag->active)
        continue;

      // check distance to the target point
      const float *end = &ag->cornerVerts[(ag->ncorners-1)*3];
      carla::geom::Vector3D dist(end[0] - ag->npos[0], end[1] - ag->npos[1], end[2] - ag->npos[2]);
      if (dist.SquaredLength() <= 2) {
        // set a new random target
        carla::geom::Location location;
        GetRandomLocationWithoutLock(location, 1);
        SetWalkerTargetIndex(i, location);
      }
    }
  }

  // get the walker current transform
  bool Navigation::GetWalkerTransform(ActorId id, carla::geom::Transform &trans) {

    // get the internal index
    auto it = _mappedId.find(id);
    if (it == _mappedId.end())
      return false;

    // get the index found
    int index = it->second;
    if (index == -1)
      return false;

    // get the walker
    const dtCrowdAgent *agent = _crowd->getAgent(index);

    if (!agent->active) {
      return false;
    }

    float baseOffset = 0.0f;
    auto it2 = _baseHeight.find(id);
    if (it2 != _baseHeight.end())
      baseOffset = it2->second;
    else
      logging::log("Nav: base offset of walker ", id, " not found");

    // set its position in Unreal coordinates
    trans.location.x = agent->npos[0];
    trans.location.y = agent->npos[2];
    trans.location.z = agent->npos[1] + baseOffset - 0.08f;   // 0.08f is a hardcoded value to get rid of some empty space

    // set its rotation
    float yaw =  atan2f(agent->dvel[2] , agent->dvel[0]) * (180.0f / static_cast<float>(M_PI));
    float shortest_angle = fmod(yaw - yaw_walkers[id] + 540.0f, 360.0f) - 180.0f;
    float rotation_speed = 4.0f;
    trans.rotation.yaw = yaw_walkers[id] + (shortest_angle * rotation_speed * static_cast<float>(_delta_seconds));

    yaw_walkers[id] = trans.rotation.yaw;
    return true;
  }

  float Navigation::GetWalkerSpeed(ActorId id) {
    // get the internal index
    auto it = _mappedId.find(id);
    if (it == _mappedId.end()) {
      return false;
    }

    // get the index found
    int index = it->second;
    if (index == -1) {
      return false;
    }

    // get the walker
    const dtCrowdAgent *agent = _crowd->getAgent(index);
    return sqrt(agent->vel[0] * agent->vel[0] + agent->vel[1] * agent->vel[1] + agent->vel[2] *
        agent->vel[2]);
  }

  bool Navigation::GetRandomLocation(carla::geom::Location &location, float maxHeight, dtQueryFilter *filter) {
    std::lock_guard<std::mutex> lock(_mutex);
    return GetRandomLocationWithoutLock(location, maxHeight, filter);
  }
  // get a random location for navigation
  bool Navigation::GetRandomLocationWithoutLock(carla::geom::Location &location, float maxHeight, dtQueryFilter *filter) {
    DEBUG_ASSERT(_navQuery != nullptr);

    // filter
    dtQueryFilter filter2;
    if (filter == nullptr) {
      filter2.setIncludeFlags(SAMPLE_POLYFLAGS_ALL ^ SAMPLE_POLYFLAGS_DISABLED);
      filter2.setExcludeFlags(0);
      filter = &filter2;
    }

    // search
    dtPolyRef randomRef { 0 };
    float point[3] { 0.0f, 0.0f, 0.0f };

    do {
      dtStatus status = _navQuery->findRandomPoint(filter, &frand, &randomRef, point);
      if (status == DT_SUCCESS) {
        // set the location in Unreal coords
        location.x = point[0];
        location.y = point[2];
        location.z = point[1];
        if (maxHeight == -1.0f || (maxHeight >= 0.0f && location.z <= maxHeight))
          break;
      }
    }
    while (1);

    return true;
  }

} // namespace nav
} // namespace carla