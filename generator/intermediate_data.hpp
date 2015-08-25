#pragma once

#include "generator/intermediate_elements.hpp"

#include "coding/file_name_utils.hpp"
#include "coding/file_reader.hpp"
#include "coding/file_writer.hpp"
#include "coding/mmap_reader.hpp"

#include "base/logging.hpp"

#include "std/algorithm.hpp"
#include "std/deque.hpp"
#include "std/exception.hpp"
#include "std/limits.hpp"
#include "std/utility.hpp"
#include "std/vector.hpp"

#include "std/fstream.hpp"

#include "defines.hpp"

/// Classes for reading and writing any data in file with map of offsets for
/// fast searching in memory by some key.
namespace cache
{

enum class EMode { Write = true, Read = false };

namespace detail
{
template <class TFile, class TValue>
class IndexFile
{
  using TKey = uint64_t;
  static_assert(is_integral<TKey>::value, "TKey is not integral type");
  using TElement = pair<TKey, TValue>;
  using TContainer = vector<TElement>;

  TContainer m_elements;
  TFile m_file;
  uint64_t m_fileSize = 0;

  static size_t constexpr kFlushCount = 1024;

  struct ElementComparator
  {
    bool operator()(TElement const & r1, TElement const & r2) const
    {
      return ((r1.first == r2.first) ? r1.second < r2.second : r1.first < r2.first);
    }
    bool operator()(TElement const & r1, TKey r2) const { return (r1.first < r2); }
    bool operator()(TKey r1, TElement const & r2) const { return (r1 < r2.first); }
  };

  static size_t CheckedCast(uint64_t v)
  {
    ASSERT_LESS(v, numeric_limits<size_t>::max(), ("Value too long for memory address : ", v));
    return static_cast<size_t>(v);
  }

public:
  IndexFile(string const & name) : m_file(name.c_str()) {}

  string GetFileName() const { return m_file.GetName(); }

  void WriteAll()
  {
    if (m_elements.empty())
      return;

    m_file.Write(&m_elements[0], m_elements.size() * sizeof(TElement));
    m_elements.clear();
  }

  void ReadAll()
  {
    m_elements.clear();
    m_fileSize = m_file.Size();
    if (m_fileSize == 0)
      return;

    LOG_SHORT(LINFO, ("Offsets reading is started for file ", GetFileName()));
    CHECK_EQUAL(0, m_fileSize % sizeof(TElement), ("Damaged file."));

    try
    {
      m_elements.resize(CheckedCast(m_fileSize / sizeof(TElement)));
    }
    catch (exception const &)  // bad_alloc
    {
      LOG(LCRITICAL, ("Insufficient memory for required offset map"));
    }

    m_file.Read(0, &m_elements[0], CheckedCast(m_fileSize));

    sort(m_elements.begin(), m_elements.end(), ElementComparator());

    LOG_SHORT(LINFO, ("Offsets reading is finished"));
  }

  void Add(TKey k, TValue const & v)
  {
    if (m_elements.size() > kFlushCount)
      WriteAll();

    m_elements.push_back(make_pair(k, v));
  }

  bool GetValueByKey(TKey key, TValue & value) const
  {
    auto it = lower_bound(m_elements.begin(), m_elements.end(), key, ElementComparator());
    if ((it != m_elements.end()) && ((*it).first == key))
    {
      value = (*it).second;
      return true;
    }
    return false;
  }

  template <class ToDo>
  void ForEachByKey(TKey k, ToDo && toDo) const
  {
    auto range = equal_range(m_elements.begin(), m_elements.end(), k, ElementComparator());
    for (; range.first != range.second; ++range.first)
    {
      if (toDo((*range.first).second))
        return;
    }
  }
};
} // namespace detail

template <EMode TMode>
class OSMElementCache
{
public:
  using TKey = uint64_t;
  using TStorage = typename conditional<TMode == EMode::Write, FileWriter, FileReader>::type;
  using TOffsetFile = typename conditional<TMode == EMode::Write, FileWriter, FileReader>::type;

protected:
  using TBuffer = vector<uint8_t>;
  TStorage m_storage;
  detail::IndexFile<TOffsetFile, uint64_t> m_offsets;
  string m_name;
  TBuffer m_data;
  bool m_preload = false;

public:
  OSMElementCache(string const & name, bool preload = false)
  : m_storage(name)
  , m_offsets(name + OFFSET_EXT)
  , m_name(name)
  , m_preload(preload)
  {
    InitStorage<TMode>();
  }

  template <EMode T>
  typename enable_if<T == EMode::Write, void>::type InitStorage() {}

  template <EMode T>
  typename enable_if<T == EMode::Read, void>::type InitStorage()
  {
    if (!m_preload)
      return;
    size_t sz = m_storage.Size();
    m_data.resize(sz);
    m_storage.Read(0, m_data.data(), sz);
  }

  template <class TValue, EMode T = TMode>
  typename enable_if<T == EMode::Write, void>::type Write(TKey id, TValue const & value)
  {
    m_offsets.Add(id, m_storage.Pos());
    m_data.clear();
    MemWriter<TBuffer> w(m_data);

    value.Write(w);

    // write buffer
    ASSERT_LESS(m_data.size(), numeric_limits<uint32_t>::max(), ());
    uint32_t sz = static_cast<uint32_t>(m_data.size());
    m_storage.Write(&sz, sizeof(sz));
    m_storage.Write(m_data.data(), sz * sizeof(TBuffer::value_type));
  }

  template <class TValue, EMode T = TMode>
  typename enable_if<T == EMode::Read, bool>::type Read(TKey id, TValue & value)
  {
    uint64_t pos = 0;
    if (!m_offsets.GetValueByKey(id, pos))
    {
      LOG_SHORT(LWARNING, ("Can't find offset in file", m_offsets.GetFileName(), "by id", id));
      return false;
    }

    uint32_t valueSize = m_preload ? *(reinterpret_cast<uint32_t *>(m_data.data() + pos)) : 0;
    size_t offset = pos + sizeof(uint32_t);

    if (!m_preload)
    {
      // in case not-in-memory work we read buffer
      m_storage.Read(pos, &valueSize, sizeof(valueSize));
      m_data.resize(valueSize);
      m_storage.Read(pos + sizeof(valueSize), m_data.data(), valueSize);
      offset = 0;
    }

    MemReader reader(m_data.data() + offset, valueSize);
    value.Read(reader);
    return true;
  }

  inline void SaveOffsets() { m_offsets.WriteAll(); }
  inline void LoadOffsets() { m_offsets.ReadAll(); }
};

/// Used to store all world nodes inside temporary index file.
/// To find node by id, just calculate offset inside index file:
/// offset_in_file = sizeof(LatLon) * node_ID
class PointStorage
{
  size_t m_processedPoint = 0;

public:
  struct LatLon
  {
    int32_t lat;
    int32_t lon;
  };
  static_assert(sizeof(LatLon) == 8, "Invalid structure size");

  struct LatLonPos
  {
    uint64_t pos;
    int32_t lat;
    int32_t lon;
  };
  static_assert(sizeof(LatLonPos) == 16, "Invalid structure size");

  inline size_t GetProcessedPoint() const { return m_processedPoint; }
  inline void IncProcessedPoint() { ++m_processedPoint; }
};

template <EMode TMode>
class RawFilePointStorage : public PointStorage
{
#ifdef OMIM_OS_WINDOWS
  using TFileReader = FileReader;
#else
  using TFileReader = MmapReader;
#endif

  typename conditional<TMode == EMode::Write, FileWriter, TFileReader>::type m_file;

  constexpr static double const kValueOrder = 1E+7;

public:
  RawFilePointStorage(string const & name) : m_file(name) {}

  template <EMode T = TMode>
  typename enable_if<T == EMode::Write, void>::type AddPoint(uint64_t id, double lat, double lng)
  {
    int64_t const lat64 = lat * kValueOrder;
    int64_t const lng64 = lng * kValueOrder;

    LatLon ll;
    ll.lat = static_cast<int32_t>(lat64);
    ll.lon = static_cast<int32_t>(lng64);
    CHECK_EQUAL(static_cast<int64_t>(ll.lat), lat64, ("Latitude is out of 32bit boundary!"));
    CHECK_EQUAL(static_cast<int64_t>(ll.lon), lng64, ("Longtitude is out of 32bit boundary!"));

    m_file.Seek(id * sizeof(ll));
    m_file.Write(&ll, sizeof(ll));

    IncProcessedPoint();
  }

  template <EMode T = TMode>
  typename enable_if<T == EMode::Read, bool>::type GetPoint(uint64_t id, double & lat,
                                                            double & lng) const
  {
    LatLon ll;
    m_file.Read(id * sizeof(ll), &ll, sizeof(ll));

    // assume that valid coordinate is not (0, 0)
    if (ll.lat != 0.0 || ll.lon != 0.0)
    {
      lat = static_cast<double>(ll.lat) / kValueOrder;
      lng = static_cast<double>(ll.lon) / kValueOrder;
      return true;
    }
    LOG(LERROR, ("Node with id = ", id, " not found!"));
    return false;
  }
};

template <EMode TMode>
class RawMemPointStorage : public PointStorage
{
  typename conditional<TMode == EMode::Write, FileWriter, FileReader>::type m_file;

  constexpr static double const kValueOrder = 1E+7;

  vector<LatLon> m_data;

public:
  RawMemPointStorage(string const & name) : m_file(name), m_data((size_t)0xFFFFFFFF)
  {
    InitStorage<TMode>();
  }

  ~RawMemPointStorage() { DoneStorage<TMode>(); }

  template <EMode T>
  typename enable_if<T == EMode::Write, void>::type InitStorage() {}

  template <EMode T>
  typename enable_if<T == EMode::Read, void>::type InitStorage()
  {
    m_file.Read(0, m_data.data(), m_data.size() * sizeof(LatLon));
  }

  template <EMode T>
  typename enable_if<T == EMode::Write, void>::type DoneStorage()
  {
    m_file.Write(m_data.data(), m_data.size() * sizeof(LatLon));
  }

  template <EMode T>
  typename enable_if<T == EMode::Read, void>::type DoneStorage() {}

  template <EMode T = TMode>
  typename enable_if<T == EMode::Write, void>::type AddPoint(uint64_t id, double lat, double lng)
  {
    int64_t const lat64 = lat * kValueOrder;
    int64_t const lng64 = lng * kValueOrder;

    LatLon & ll = m_data[id];
    ll.lat = static_cast<int32_t>(lat64);
    ll.lon = static_cast<int32_t>(lng64);
    CHECK_EQUAL(static_cast<int64_t>(ll.lat), lat64, ("Latitude is out of 32bit boundary!"));
    CHECK_EQUAL(static_cast<int64_t>(ll.lon), lng64, ("Longtitude is out of 32bit boundary!"));

    IncProcessedPoint();
  }

  template <EMode T = TMode>
  typename enable_if<T == EMode::Read, bool>::type GetPoint(uint64_t id, double & lat,
                                                            double & lng) const
  {
    LatLon const & ll = m_data[id];
    // assume that valid coordinate is not (0, 0)
    if (ll.lat != 0.0 || ll.lon != 0.0)
    {
      lat = static_cast<double>(ll.lat) / kValueOrder;
      lng = static_cast<double>(ll.lon) / kValueOrder;
      return true;
    }
    LOG(LERROR, ("Node with id = ", id, " not found!"));
    return false;
  }
};

template <EMode TMode>
class MapFilePointStorage : public PointStorage
{
  typename conditional<TMode == EMode::Write, FileWriter, FileReader>::type m_file;
  unordered_map<uint64_t, pair<int32_t, int32_t>> m_map;

  constexpr static double const kValueOrder = 1E+7;

public:
  MapFilePointStorage(string const & name) : m_file(name + ".short") { InitStorage<TMode>(); }

  template <EMode T>
  typename enable_if<T == EMode::Write, void>::type InitStorage() {}

  template <EMode T>
  typename enable_if<T == EMode::Read, void>::type InitStorage()
  {
    LOG(LINFO, ("Nodes reading is started"));

    uint64_t const count = m_file.Size();

    uint64_t pos = 0;
    while (pos < count)
    {
      LatLonPos ll;
      m_file.Read(pos, &ll, sizeof(ll));

      m_map.emplace(make_pair(ll.pos, make_pair(ll.lat, ll.lon)));

      pos += sizeof(ll);
    }

    LOG(LINFO, ("Nodes reading is finished"));
  }

  void AddPoint(uint64_t id, double lat, double lng)
  {
    int64_t const lat64 = lat * kValueOrder;
    int64_t const lng64 = lng * kValueOrder;

    LatLonPos ll;
    ll.pos = id;
    ll.lat = static_cast<int32_t>(lat64);
    ll.lon = static_cast<int32_t>(lng64);
    CHECK_EQUAL(static_cast<int64_t>(ll.lat), lat64, ("Latitude is out of 32bit boundary!"));
    CHECK_EQUAL(static_cast<int64_t>(ll.lon), lng64, ("Longtitude is out of 32bit boundary!"));
    m_file.Write(&ll, sizeof(ll));

    IncProcessedPoint();
  }

  bool GetPoint(uint64_t id, double & lat, double & lng) const
  {
    auto i = m_map.find(id);
    if (i == m_map.end())
      return false;
    lat = static_cast<double>(i->second.first) / kValueOrder;
    lng = static_cast<double>(i->second.second) / kValueOrder;
    return true;
  }
};

}  // namespace cache
