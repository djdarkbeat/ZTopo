/*
  ZTopo --- a viewer for topographic maps
  Copyright (C) 2010 Peter Hawkins
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringBuilder>
#include <db_cxx.h>
#include "tilecache.h"
#include "consts.h"

using namespace boost::intrusive;

static const int numWorkerThreads = 1;
static const int maxThreadWaitTime = 1000; // Maximum time to wait for a thread to die

static const int maxBufferSize = 500000;

static const QNetworkRequest::Attribute keyAttr(QNetworkRequest::User);


namespace Cache {
  typedef QPair<Key, u_int32_t> NetworkReqKey;

  static const int kindShift = 56;
  Kind keyKind(Key k) {
    return Kind(k >> kindShift);
  }
  qkey keyQuad(Key k) {
    return qkey(k & ((Key(1) << kindShift) - 1));
  }
  Key tileKey(qkey k) {
    return (Key(TileKind) << kindShift) | k;
  }
  Key indexKey(qkey k) {
    return (Key(IndexKind) << kindShift) | k;
  }

  bool isInMemory(State s)
  {
    return (s == DiskAndMemory || s == MemoryOnly || s == Saving);
  }
  
  Entry::Entry(Key aKey) 
    : key(aKey), memSize(0), diskSize(0), pixmap(NULL), indexData(NULL),
      state(Invalid), inUse(false)
  {
  }
  
  IORequest::IORequest(IORequestKind k, Key t, QVariant d)
    : kind(k), tile(t), data(d)
  {
  }


  NetworkRequest::NetworkRequest(Key k, qkey q, u_int32_t off, u_int32_t l)
    : key(k), qid(q), offset(off), len(l)
  {
  }
  

  IOThread::IOThread(Cache *v, QObject *parent)
    : QThread(parent), cache(v)
  {
  }

  void IOThread::run()
  {
    forever {
      cache->tileQueueMutex.lock();
      while (cache->tileQueue.isEmpty()) {
        cache->tileQueueCond.wait(&cache->tileQueueMutex);
      }
      IORequest req = cache->tileQueue.dequeue();
      cache->tileQueueMutex.unlock();
      
      
      switch (req.kind) {
      case LoadObject: {
        QByteArray data;
        QByteArray indexData;
        QImage tileData;
        if (cache->objectDb) {
          // Do one Get to retrieve the object size, then another to retrieve the object
          Dbt dbKey(&req.tile, sizeof(Key));
          Dbt dbData;
          dbData.set_data(NULL);
          dbData.set_ulen(0);
          dbData.set_flags(DB_DBT_USERMEM);
          try {
            cache->objectDb->get(NULL, &dbKey, &dbData, 0);
          }
          catch (DbMemoryException &e) {
            // That's what we expected...
          }

          data.resize(dbData.get_size());
          dbData.set_data(data.data());
          dbData.set_ulen(dbData.get_size());
          dbData.set_flags(DB_DBT_USERMEM);
          int ret = cache->objectDb->get(NULL, &dbKey, &dbData, 0);
          if (ret != 0) {
            qWarning() << "Error loading cached object " << req.tile;
            data.clear();
          }
          else {
            writeMetadata(req.tile, data.size());
            Cache::decompressObject(req.tile, data, indexData, tileData);
          }
        }
        //        emit(objectLoadedFromDisk(req.tile, indexData, tileData));
        QCoreApplication::postEvent(cache, new NewDataEvent(req.tile, data, indexData, tileData),
                                    Qt::LowEventPriority);
        break;
      }
        
      case SaveObject: {
        //        qDebug() << "saving " << req.tile;
        QByteArray data = req.data.value<QByteArray>();
        Dbt dbKey(&req.tile, sizeof(Key));
        Dbt dbData((void *)data.constData(), data.size());
        int ret = -1;
        if (cache->objectDb) {
          ret = cache->objectDb->put(NULL, &dbKey, &dbData, 0);
          if (ret != 0) {
            qWarning() << "Cache database put failed with return code " << ret;
          } else {
            writeMetadata(req.tile, data.size());
          }
        }
        emit(objectSavedToDisk(req.tile, ret == 0));
        break;
      }
        
      case DeleteObject: {
        // qDebug() << "deleting " << req.tile;
        if (cache->objectDb && cache->timestampDb) {
          Dbt dbKey(&req.tile, sizeof(Key));
          int ret = cache->objectDb->del(NULL, &dbKey, 0);
          if (ret != 0) {
            qWarning() << "Cache delete " << req.tile << " failed with return code " 
                       << ret;
          }
          ret = cache->timestampDb->del(NULL, &dbKey, 0);
          if (ret != 0) {
            qWarning() << "Cache timestamp delete " << req.tile << " failed with return code " 
                       << ret;
          }


        }      
        break;
      }
        
      case UpdateObjectMetadata: {
        writeMetadata(req.tile, req.data.toUInt());
        break;
      }

      case ClearCache: {
        u_int32_t count;
        if (cache->objectDb) {
          cache->objectDb->truncate(NULL, &count, 0);
          cache->objectDb->compact(NULL, NULL, NULL, NULL, DB_FREE_SPACE, NULL);
        }
        if (cache->timestampDb) {
          cache->timestampDb->truncate(NULL, &count, 0);
          cache->timestampDb->compact(NULL, NULL, NULL, NULL, DB_FREE_SPACE, NULL);
        }
        break;
      }
        
      case TerminateThread:
        return;  // Thread is done
        
      default:
        qFatal("Unknown IO request type"); // Unknown IO request type
      }
    }
  }

  void IOThread::writeMetadata(Key key, u_int32_t size)
  {
    u_int32_t timeSize[2] = { std::time(NULL), size };
    Dbt dbKey(&key, sizeof(Key));
    Dbt dbData(&timeSize, sizeof(timeSize));
    int ret = -1;
    if (cache->timestampDb) {
      ret = cache->timestampDb->put(NULL, &dbKey, &dbData, 0);
      if (ret != 0) {
        qWarning() << "Timestamp put failed with return code " << ret;
      }
    }
  }
  
  
  Cache::Cache(Map *m, int maxMem, int maxDisk, const QString &cp)
    : map(m), cachePath(cp), manager(this), maxMemCache(maxMem), 
      maxDiskCache(maxDisk), memLRUSize(0), diskLRUSize(0),
      diskCacheHits(0), diskCacheMisses(0), memCacheHits(0), memCacheMisses(0), numNetworkReqs(0),
      networkReqSize(0), dbEnv(NULL), objectDb(NULL), timestampDb(NULL)
  {
    try {
      u_int32_t envFlags = DB_CREATE | DB_INIT_MPOOL;
      dbEnv = new DbEnv(0);
      dbEnv->open(cachePath.path().toLatin1().data(), envFlags, 0);
      
      objectDb = new Db(dbEnv, 0);
      timestampDb = new Db(dbEnv, 0);
      
      u_int32_t dbFlags = DB_CREATE;
      QString objectDbName = map->id() % ".db";
      QString timestampDbName = map->id() % "-timestamp.db";
      objectDb->open(NULL, objectDbName.toLatin1().data(), NULL, DB_BTREE, dbFlags, 0);
      timestampDb->open(NULL, timestampDbName.toLatin1().data(), NULL, DB_BTREE, 
                        dbFlags, 0);
      
    } catch (DbException &e) {
      if (timestampDb) { delete timestampDb; timestampDb = NULL; }
      if (objectDb) { delete objectDb; objectDb = NULL; }
      if (dbEnv) { delete dbEnv; dbEnv = NULL; }
      qWarning("Database exception opening tile cache environment %s", 
               cachePath.path().toLatin1().data());
    } catch (std::exception &e) {
      if (timestampDb) { delete timestampDb; timestampDb = NULL; }
      if (objectDb) { delete objectDb; objectDb = NULL; }
      if (dbEnv) { delete dbEnv; dbEnv = NULL; }
      qWarning("Database exception opening tile cache environment %s", 
               cachePath.path().toLatin1().data());
    }
    
    initializeCacheFromDatabase();
    
    for (int i = 0; i < numWorkerThreads; i++) {
      IOThread *ioThread = new IOThread(this, this);
      ioThreads << ioThread;
      //      connect(ioThread, SIGNAL(objectLoadedFromDisk(Key, QByteArray, QImage)),
      //              this, SLOT(objectLoadedFromDisk(Key, QByteArray, QImage)));
      connect(ioThread, SIGNAL(objectSavedToDisk(Key, bool)),
              this, SLOT(objectSavedToDisk(Key, bool)));
      
      ioThread->start();
    }
    
    connect(&manager, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(objectReceivedFromNetwork(QNetworkReply*)));
  }
  
  Cache::~Cache()
  {
    // Terminate worker threads
    tileQueueMutex.lock();
    for (int i = 0; i < ioThreads.size(); i++) {
      tileQueue.enqueue(IORequest(TerminateThread, 0, QVariant()));
    }
    tileQueueCond.wakeAll();
    tileQueueMutex.unlock();
    foreach (IOThread *t, ioThreads) {
      t->wait(maxThreadWaitTime);
      delete t;
    }
    
    // Report cache statistics
    qDebug() << "Memory cache hits: " << memCacheHits << " misses: " << memCacheMisses
             << " (" << 
      qreal(memCacheHits * 100.0) / qreal(memCacheHits + memCacheMisses) << "%)";
    qDebug() << "Disk cache hits: " << diskCacheHits << " misses: " << diskCacheMisses
             << " (" << 
      qreal(diskCacheHits * 100.0) / qreal(diskCacheHits + diskCacheMisses) << "%)";
    qDebug() << "Network requests: " << numNetworkReqs << " size: " << networkReqSize 
             << " (" << qreal(networkReqSize) / qreal(numNetworkReqs) << " per request)";
    
    // Clear the cache
    foreach (Entry *t, cacheEntries) {
      delete t;
    }
    cacheEntries.clear();
    
    // Close databases
    try {
      if (objectDb)    { objectDb->close(0);    delete objectDb; }
      if (timestampDb) { timestampDb->close(0); delete timestampDb; }
      if (dbEnv)       { dbEnv->close(0);       delete dbEnv; }
    } catch (DbException &e) {
      qWarning() << "Database exception closing tile cache";
      delete dbEnv;
    } catch (std::exception &e) {
      qWarning() << "IO exception closing tile cache";
      delete dbEnv;
    }
  }
  
  void Cache::setCacheSizes(int mem, int disk) {
    maxMemCache = mem;
    maxDiskCache = disk;
    purgeMemLRU();
  }
  
  typedef QPair<u_int32_t, Key> EntryTime;
  void Cache::initializeCacheFromDatabase()
  {
    if (!objectDb || !timestampDb) return;
    
    Dbc *cursor;
    Dbt key, data;
    Key q;
    int ret;
    
    // Read objects
    /*    objectDb->cursor(NULL, &cursor, 0);
    if (!cursor) {
      qWarning() << "objectDb.cursor() failed";
      return;
    }
    
    key.set_data(&q);
    key.set_ulen(sizeof(Key));
    key.set_flags(DB_DBT_USERMEM);
    data.set_data(NULL);
    data.set_ulen(0);
    while ((ret = cursor->get(&key, &data, DB_NEXT)) == 0) {
      assert(key.get_size() == sizeof(Key));
    }
    cursor->close();
    */
    // Read timestamps to get LRU order
    timestampDb->cursor(NULL, &cursor, 0);
    if (!cursor) {
      qWarning() << "timestampDb.cursor() failed";
      return;
    }
    
    u_int32_t timeSize[2];
    key.set_data(&q);
    key.set_ulen(sizeof(Key));
    key.set_flags(DB_DBT_USERMEM);
    data.set_data(&timeSize);
    data.set_ulen(sizeof(timeSize));
    data.set_flags(DB_DBT_USERMEM);
    
    QVector<EntryTime> tiletimes;
    while ((ret = cursor->get(&key, &data, DB_NEXT)) == 0) {
      assert(key.get_size() == sizeof(Key) && data.get_size() == sizeof(timeSize)); 
      Entry *e = new Entry(q);
      e->diskSize = timeSize[1];
      e->state = Disk;
      cacheEntries[q] = e;
      tiletimes << EntryTime(timeSize[0], q);
    }
    cursor->close();
    qSort(tiletimes);
    foreach (const EntryTime &t, tiletimes) {
      q = t.second;
      Entry *e = cacheEntries.value(q);
      addToDiskLRU(*e);
    }
  }
  
  void Cache::postIORequest(const IORequest &req)
  {
    tileQueueMutex.lock();
    tileQueue.enqueue(req);
    tileQueueCond.wakeOne();
    tileQueueMutex.unlock();
  }


  void Cache::addToDiskLRU(Entry &e)
  {
    assert(!e.is_linked() && e.state == Disk);
    diskLRUSize += e.diskSize;
    diskLRU.push_back(e);
  }
  
  void Cache::removeFromDiskLRU(Entry &e)
  {
    assert(e.is_linked() && e.state == Disk);
    diskLRUSize -= e.diskSize;
    e.unlink();
  }
  
  void Cache::addToMemLRU(Entry &e)
  {
    assert(!e.is_linked());
    assert(e.state == DiskAndMemory || e.state == MemoryOnly);
    assert(e.pixmap != NULL || !e.indexData.isEmpty());
    assert(!e.inUse);
    memLRUSize += e.memSize;
    memLRU.push_back(e);
  }
  
  void Cache::removeFromMemLRU(Entry &e)
  {
    assert(e.is_linked());
    memLRUSize -= e.memSize;
    e.unlink();
  }
  
  void Cache::purgeDiskLRU()
  {
    qint64 maxDiskLRUSize = qint64(maxDiskCache) * qint64(bytesPerMb);
    while (diskLRUSize > maxDiskLRUSize) {
      assert(!diskLRU.empty());
      Entry &e = diskLRU.front();
      diskLRU.pop_front();
      diskLRUSize -= e.diskSize;
      
      assert(e.state == Disk && e.pixmap == NULL);
      
      postIORequest(IORequest(DeleteObject, e.key, QVariant()));
      cacheEntries.remove(e.key);
      delete &e;
    }
  }

  void Cache::emptyDiskCache()
  {
    postIORequest(IORequest(ClearCache, 0, QVariant()));
    while (!diskLRU.empty()) {
      Entry &e = diskLRU.front();
      diskLRU.pop_front();
      
      assert(e.state == Disk && e.pixmap == NULL);
      
      cacheEntries.remove(e.key);
      delete &e;
    }

    diskLRUSize = 0;
  }
  
  void Cache::purgeMemLRU()
  {
    time_t tm = std::time(NULL);
    while (memLRUSize > ((unsigned int)maxMemCache) * bytesPerMb) {
      assert(!memLRU.empty());
      Entry &e = memLRU.front();
      
      memLRU.pop_front();
      memLRUSize -= e.memSize;
      if (e.pixmap) { 
        delete e.pixmap;
        e.pixmap = NULL;
      }
      e.indexData.clear();
      
      if (e.state == DiskAndMemory) {
        e.state = Disk;
        postIORequest(IORequest(UpdateObjectMetadata, e.key, QVariant(e.diskSize)));
        addToDiskLRU(e);
      } else {
        assert(e.state == MemoryOnly);
        cacheEntries.remove(e.key);
        delete &e;
      }
    }
    purgeDiskLRU();
  }
  
  void Cache::decompressObject(Key key, const QByteArray &compressed, QByteArray &indexData, QImage &tileData)
  {
    switch (keyKind(key)) {
    case IndexKind:
      indexData = qUncompress(compressed);
      break;
      
    case TileKind: 
      tileData = QImage::fromData(compressed);
      break;

    default: qFatal("Unknown key kind in decompressObject");
    }
  }
    
  bool Cache::loadObject(Entry *e, const QByteArray &indexData, const QImage &tileData)
  {
    switch (keyKind(e->key)) {
    case IndexKind: {
      e->indexData = indexData;
      e->memSize = e->indexData.size();

      qkey q;
      int layer;
      quadKeyUnpack(map->maxLevel(), keyQuad(e->key), q, layer);
      int level = log2_int(q) / 2;

      int step = map->layer(layer).indexLevelStep();
      assert(level % step == 0);

      int maxLevel = std::min(level + step, map->layer(layer).maxLevel());

      // The formula sums the series 4 * (4^1 + 4^2 + 4^3 + ...) which is the size
      // in bytes of a complete 4-way tree without a root node of 32-bit integers.
      // See the online encyclopedia of integer sequences :-)
      if (e->indexData.size() !=
             (((1 << (2 * (maxLevel - level + 2))) - 1) / 3 - 5))
        return false;

      return true;
    }

    case TileKind: {
      if (tileData.isNull()) return false;
      QPixmap *p = new QPixmap(QPixmap::fromImage(tileData));
      e->pixmap = p;
      e->memSize = p->size().width() * p->size().height() * p->depth() / 8;
      return true;
    }

    default: qFatal("Unknown kind in decompressObject");
    }
  }

  void Cache::maybeFetchIndexPendingTiles() {
    // There might be tiles waiting on this index. Run over the list of
    // objects waiting for an index.
    //    qDebug() << "Reconsidering index pending list";
    CacheList::iterator it(indexPending.begin()), itend(indexPending.end());
    while (it != itend) {
      Entry &e = *it;
      it++;
      maybeMakeNetworkRequest(&e);
    }
    flushNetworkRequests();
  }

  static const QEvent::Type newDataEventType = QEvent::Type(QEvent::registerEventType());
  NewDataEvent::NewDataEvent(Key key, const QByteArray &data, const QByteArray &indexData, 
                             const QImage &tileData) 
    : QEvent(newDataEventType), fKey(key), fData(data), fIndexData(indexData), fTileData(tileData)
  {
  }

  bool Cache::event(QEvent *ev)
  {
    if (ev->type() == newDataEventType) {

      NewDataEvent *nev = (NewDataEvent *)ev;
      Key key = nev->key();
      const QByteArray &data = nev->data();
      const QByteArray &indexData = nev->indexData();
      const QImage &tileData = nev->tileData();

      assert(cacheEntries.contains(key));
      Entry *e = cacheEntries.value(key);
      assert(e->state == Loading || e->state == NetworkPending);

      bool ok = !indexData.isEmpty() || !tileData.isNull();
      if (ok) {
        ok = loadObject(e, indexData, tileData);
      }
      
      e->diskSize = data.size();

      switch (e->state) {
      case Loading:
        if (!ok) {
          e->state = Disk;
          if (dbEnv) {
            qDebug() << "WARNING: Could not read disk object " << key;
          }
          
          addToDiskLRU(*e);
        } else {
          // qDebug() << "received " << e->key << " from disk";
          e->state = DiskAndMemory;
          
          if (e->inUse) {
            memInUse.push_back(*e);
          } else {
            addToMemLRU(*e);
            purgeMemLRU();
          }
        }
        break;
      case NetworkPending:
        if (ok) {
          // qDebug() << "received " << e->key << " from network";
          e->state = Saving;
      
          postIORequest(IORequest(SaveObject, key, data));
        } else {
          // We had a network error; we have no way to restore the tile to a valid
          // state so we just dump it. If it is wanted again it will be requested again.
          qDebug() << "WARNING: Error decoding network object " << key;
      
          cacheEntries.remove(key);
          delete e;
        }
        break;
      }

      if (ok) {
        if (keyKind(key) == IndexKind) {
          maybeFetchIndexPendingTiles();
        }
        if (e->inUse) {
          emit(tileLoaded());
        }
      }
      return true;
    }
    return QObject::event(ev);
  }


void Cache::objectSavedToDisk(Key q, bool success)
{
  assert(cacheEntries.contains(q));
  Entry *e = cacheEntries.value(q);
  assert(e->state == Saving && !e->is_linked());

  if (success) {
    e->state = DiskAndMemory;
  } else {
    // Saving failed. We'll just leave the file in the memory cache
    e->state = MemoryOnly;

    if (dbEnv) {
      qDebug() << "WARNING: Could not save tile to disk " << q;
    }
  }
  if (e->inUse) {
    memInUse.push_back(*e);
  } else {
    addToMemLRU(*e);
    purgeMemLRU();
  }
}

void Cache::objectReceivedFromNetwork(QNetworkReply *reply)
{
  QNetworkRequest req = reply->request();
  QList<NetworkReqKey> *reqKeys = 
    (QList<NetworkReqKey> *) req.attribute(keyAttr).value<void *>();

  QByteArray data;
  bool ok;
  ok = (reply->error() == QNetworkReply::NoError);
  if (ok) {
    data = reply->readAll();
    ok = !data.isEmpty();
  }

  int pos = 0;
  foreach (const NetworkReqKey &r, *reqKeys) {
    Key key = r.first;

    assert(r.second != 0 || reqKeys->size() == 1);
    int len = (r.second == 0) ? data.size() : r.second;
      
    assert(cacheEntries.contains(key));
    Entry *e = cacheEntries.value(key);
    assert(e->state == NetworkPending);
   


    ok = ok && (pos + len <= data.size());
    if (ok) {
      QByteArray indexData;
      QImage tileData;

      QByteArray subData(data.constData() + pos, len);
      decompressObject(key, subData, indexData, tileData);
      //      ok = loadObject(e, indexData, tileData);
      //      e->diskSize = subData.size();
      QCoreApplication::postEvent(this, new NewDataEvent(key, subData, indexData, tileData), 
                                  Qt::LowEventPriority);
    }

    if (!ok) {
      // We had a network error; we have no way to restore the tile to a valid
      // state so we just dump it. If it is wanted again it will be requested again.
      qDebug() << "WARNING: Error reading network object " << key << ": " << req.url().toString() << " " 
               << reply->error() << reply->errorString();
      
      cacheEntries.remove(key);
      delete e;
    }

    pos += len;
  }
  delete reqKeys;
}


  // Find tiles that were previously in use that are no longer in use.
  void Cache::pruneObjects(const QList<QRect> &rects)
  {
    CacheList::iterator it(memInUse.begin()), itend(memInUse.end());
    
    while (it != itend) {
      Entry &e = *it;
      it++;
      assert((e.state == DiskAndMemory || e.state == MemoryOnly) && e.inUse);
        
      qkey q = keyQuad(e.key);
      Tile tile(q, map->maxLevel());
      QRect r = map->tileToMapRect(tile);
      
      bool inUse = false;
      foreach (const QRect &vis, rects) {
        inUse = inUse || r.intersects(vis);
      }
      if (!inUse) {
        e.unlink();
        e.inUse = false;
        addToMemLRU(e);
      }
    }
    purgeMemLRU();
  }


  bool Cache::parentIndex(qkey key, qkey &index, qkey &tile)
  {
    int layer;
    qkey q;
    quadKeyUnpack(map->maxLevel(), key, q, layer);
    int level = log2_int(q) / 2;

    if (level == 0) return false;
    
    int step = map->layer(layer).indexLevelStep();
    int idxLevel = ((level - 1) / step) * step;

    assert(idxLevel == 0 || idxLevel == 6);
    qkey qidx = (q & ((1 << (idxLevel * 2)) - 1)) | (1 << (idxLevel * 2));
    tile = q >> (idxLevel * 2);
    
    index = quadKeyPack(map->maxLevel(), qidx, layer);
    return true;
  }


  inline int tileSize(u_int32_t *data, int off, int len) {
    assert(off < len);
    u_int32_t ret = data[off];
    
    off = 4 * (off + 1);
    if (off + 4 <= len) {
      for (int i = 0; i < 4; i++) {
        ret -= data[off + i];
      }
    }
    return ret;
  }
  
  void Cache::findTileRange(qkey q, Entry *e, u_int32_t &offset, u_int32_t &len)
  {
    u_int32_t *data = (u_int32_t *)e->indexData.constData();
    if (e->indexData.isEmpty()) {
      // Dummy index
      offset = 0;
      len = 0;
      return;
    }
    int datalen = e->indexData.size() / 4;
    
    offset = 0;
    int pos = 0;
    // For all levels except the last one...
    while (q > 7) {
      int digit = q & 3;
      int i;
      for (i = 0; i < digit; i++) {
        offset += data[pos + i];
      }
      for (; i < 4; i++) {
        offset += tileSize(data, pos + i, datalen);
      }
      
      q >>= 2;
      pos = 4 * (pos + digit + 1);
    }
    
    // The last level
    int digit = q & 3;
    for (int i = 0; i < digit; i++) {
      offset += tileSize(data, pos + i, datalen);
    }
    len = tileSize(data, pos + digit, datalen);
  }

  bool Cache::requestTiles(const QList<Tile> &tiles)
  {
    bool present = true;
    foreach (const Tile& tile, tiles) {
      qkey q = tile.toQuadKey(map->maxLevel());
      bool tilePresent = requestObject(tileKey(q));
      //if (!tilePresent) { qDebug() << "tile " << tile.x << " " << tile.y << " " << tile.level << " " << 
      //    tileKey(q) << " present " << tilePresent; }
      present = present && tilePresent;
    }
    flushNetworkRequests();
    return present;
  }

  void Cache::flushNetworkRequests() {
    QString baseUrl(map->baseUrl().toString());

    qSort(networkRequests);

    int i = 0;
    while (i < networkRequests.size()) {
      NetworkRequest &n = networkRequests[i];

      qkey qid = n.qid;
      Kind kind = keyKind(n.key);
      u_int32_t offset = n.offset;

      QList<NetworkReqKey> *bundle = new QList<NetworkReqKey>();
      while (i < networkRequests.size() && keyKind(networkRequests[i].key) == kind &&
             networkRequests[i].qid == qid &&
             networkRequests[i].offset == offset) {
        u_int32_t len = networkRequests[i].len;
        *bundle << NetworkReqKey(networkRequests[i].key, len);
        offset += len;
        i++;
      }

      QString url = baseUrl % "/" % map->indexFile(n.qid) % 
        ((kind == IndexKind) ? ".idxz" : ".dat");
      QNetworkRequest req;
      req.setUrl(QUrl(url));
      req.setAttribute(keyAttr, qVariantFromValue((void *)bundle));

      // qDebug() << "req " << url << " off " << n.offset << " len "<< offset - n.offset << " coalesce " << bundle->size();
      if (n.len >= 0) {
        QString rangeHdr = "bytes=" % QString::number(uint(n.offset)) % "-" %
          QString::number(uint(offset - 1));
        req.setRawHeader(QByteArray("Range"), rangeHdr.toLatin1());
      }

      numNetworkReqs++;
      networkReqSize += offset - n.offset;
      manager.get(req);
    }

    networkRequests.clear();
  }

  void Cache::maybeMakeNetworkRequest(Entry *e)
  {
    assert(e->state == IndexPending && e->is_linked());

    qkey q = keyQuad(e->key);
    qkey qidx, qtile;    
    u_int32_t offset, len;

    // Ensure the parent index (if any) is loaded
    if (parentIndex(q, qidx, qtile)) {
      Key idxKey = indexKey(qidx);
      //      qDebug() << "qidx " << qidx << " qtile " << qtile << " idxkey " << idxKey  << " file " << 
        map->indexFile(qidx);

      requestObject(idxKey);
      Entry *idx = cacheEntries.value(idxKey);
    
      if (!isInMemory(idx->state)) {
        return;
      }

      // Find the tile range of the immediate parent (even for indices).
      // If it's empty, we're done here.
      findTileRange(qtile, idx, offset, len);
      if (len == 0) {
        // qDebug() << "request " << e->key << " out of range";
        // Object doesn't exist (it's outside the map data), so we're done
        e->state = MemoryOnly;
        e->pixmap = new QPixmap();
        e->unlink();
        memInUse.push_back(*e);
        return;
      }
    } else {
      assert(keyKind(e->key) == IndexKind);
    }

    e->unlink();
    e->state = NetworkPending;

    // qDebug() << "request " << e->key << " network get";
    //    qDebug() << "key " << e->key << " q " << q << " " << baseUrl;
    switch (keyKind(e->key)) {
    case TileKind: {
      networkRequests << NetworkRequest(e->key, qidx, offset, len);
      break;
    }
    case IndexKind: {
      networkRequests << NetworkRequest(e->key, q, 0, 0);
      break;
    }
    default: qFatal("Unknown kind in requestObject");
    }
    //    qDebug() << "fetching url " << req.url();
  }

  bool Cache::requestObject(Key key)
  {
    bool present;
    if (cacheEntries.contains(key)) {
      Entry *e = cacheEntries.value(key);
      switch (e->state) {
      case Disk: {
        // qDebug() << "request " << key << " disk cache hit";
        diskCacheHits++;
        memCacheMisses++;
        removeFromDiskLRU(*e);
        e->state = Loading;  
        e->inUse = true;
        
        postIORequest(IORequest(LoadObject, key, QVariant()));
        present = false;
        break;
      }      
      case Loading:
      case IndexPending:
      case NetworkPending:
        // qDebug() << "request " << key << " in transition";
        e->inUse = true;
        present = false;
        break;

      case Saving:
        // qDebug() << "request " << key << " in transition";
        e->inUse = true;
        present = true;
        break;
        
      case DiskAndMemory:
      case MemoryOnly:
        // qDebug() << "request " << key << " memory hit";
        if (!e->inUse) {
          memCacheHits++;
          removeFromMemLRU(*e);
          memInUse.push_back(*e);
          e->inUse = true;
        }
        present = true;
        break;
        
      default: abort(); // Unreachable
      }
    } else {
      // Load object from the network.
      memCacheMisses++;
      diskCacheMisses++;
      // qDebug() << "request " << key << " cache miss";
      Entry *e = new Entry(key);
      cacheEntries[key] = e;
      e->state = IndexPending;
      e->inUse = true;
      present = false;
      indexPending.push_back(*e);
      maybeMakeNetworkRequest(e);
    } 
    return present;
  }


bool Cache::getTile(const Tile &tile, QPixmap &p) const
{
  qkey q = tile.toQuadKey(map->maxLevel());
  Key key = tileKey(q);
  if (cacheEntries.contains(key)) {
    Entry *e = cacheEntries.value(key);
    assert(keyKind(e->key) == TileKind);
    if (isInMemory(e->state)) {
      p = *e->pixmap;
      return true;
    }
  } 
  return false;
}

} // namespace "Cache"
