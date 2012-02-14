#ifndef WLCOMPILECHECK
#define WLCOMPILECHECK(name) //
#endif

namespace GaloisRuntime {
namespace WorkList {
namespace Alt {

class ChunkHeader {
public:
  ChunkHeader* next;
  ChunkHeader() :next(0) {}
};

template<typename T, int chunksize>
class Chunk : public ChunkHeader {
  T data[chunksize];
  int num;
public:
  Chunk() :num(0) {}
  std::pair<bool, T> pop() {
    if (num)
      return std::make_pair(true, data[--num]);
    else
      return std::make_pair(false, T());
  }
  bool push(T val) {
    if (num < chunksize) {
      data[num++] = val;
      return true;
    }
    return false;
  }
  template<typename Iter>
  Iter push(Iter b, Iter e) {
    while (b != e && num < chunksize)
      data[num++] = *b++;
    return b;
  }
  bool empty() const { 
    return num == 0;
  }
  bool full() const {
    return num == chunksize;
  }
};

class LIFO_SB : private boost::noncopyable {
  LL::PtrLock<ChunkHeader*, true> head;

public:

  bool empty() const {
    return !head.getValue();
  }

  void push(ChunkHeader* val) {
    ChunkHeader* oldhead = 0;
    do {
      oldhead = head.getValue();
      val->next = oldhead;
    } while (!head.CAS(oldhead, val));
  }

  void pushi(ChunkHeader* val) {
    push(val);
  }

  ChunkHeader* pop() {
    //lock free Fast path (empty)
    if (empty()) return 0;
    
    //Disable CAS
    head.lock();
    ChunkHeader* C = head.getValue();
    if (!C) {
      head.unlock();
      return 0;
    }
    head.unlock_and_set(C->next);
    C->next = 0;
    return C;
  }

  //returns a chain
  ChunkHeader* steal(LIFO_SB& victim) {
    //lock free Fast path (empty)
    if (victim.empty()) return 0;
    
    //Disable CAS
    if (!victim.head.try_lock())
      return 0;
    ChunkHeader* C = victim.head.getValue();
    if (!C) {
      victim.head.unlock();
      return 0;
    }
    victim.head.unlock_and_set(C->next);
    C->next = 0;
    return C;
  }
};

class LevelLocalAlt : private boost::noncopyable {
  PerLevel<LIFO_SB> local;
  
public:
  void push(ChunkHeader* val) {
    local.get().push(val);
  }

  void pushi(ChunkHeader* val) {
    push(val);
  }

  ChunkHeader* pop() {
    return local.get().pop();
  }
};

class LevelStealingAlt : private boost::noncopyable {
  PerLevel<LIFO_SB> local;
  
public:
  void push(ChunkHeader* val) {
    local.get().push(val);
  }

  void pushi(ChunkHeader* val) {
    push(val);
  }

  ChunkHeader* pop() {
    LIFO_SB& me = local.get();

    ChunkHeader* ret = me.pop();
    if (ret)
      return ret;
    
    //steal
    int id = local.myEffectiveID();
    for (int i = 0; i < local.size(); ++i) {
      ++id;
      id %= local.size();
      ret = me.steal(local.get(id));
      if (ret)
	break;
    }
      // if (id) {
      // 	--id;
      // 	id %= local.size();
      // 	ret = me.steal(local.get(id));
      // }
      //      myLock.unlock();
    return ret;
  }
};

template<typename InitWl, typename RunningWl>
class InitialQueue : private boost::noncopyable {
  InitWl global;
  RunningWl local;
public:
  void push(ChunkHeader* val) {
    local.push(val);
  }

  void pushi(ChunkHeader* val) {
    global.pushi(val);
  }

  ChunkHeader* pop() {
    ChunkHeader* ret = local.pop();
    if (ret)
      return ret;
    return global.pop();
  }
};


template<typename gWL = LIFO_SB, int chunksize = 64, typename T = int>
class ChunkedAdaptor : private boost::noncopyable {
  typedef Chunk<T, chunksize> ChunkTy;

  MM::FixedSizeAllocator heap;

  PerCPU<ChunkTy*> data;

  gWL worklist;

  ChunkTy* mkChunk() {
    return new (heap.allocate(sizeof(ChunkTy))) ChunkTy();
  }
  
  void delChunk(ChunkTy* C) {
    C->~ChunkTy();
    heap.deallocate(C);
  }

public:
  template<typename Tnew>
  struct retype {
    typedef ChunkedAdaptor<gWL, chunksize, Tnew> WL;
  };

  typedef T value_type;

  ChunkedAdaptor() : heap(sizeof(ChunkTy)) {
    for (unsigned int i = 0; i  < data.size(); ++i)
      data.get(i) = 0;
  }

  bool push(value_type val) {
    ChunkTy*& n = data.get();
    //Simple case, space in current chunk
    if (n && n->push(val))
      return true;
    //full chunk, push
    if (n)
      worklist.push(static_cast<ChunkHeader*>(n));
    //get empty chunk;
    n = mkChunk();
    //There better be something in the new chunk
    n->push(val);
    return true;
  }

  bool pushi(value_type val) {
    return push(val);
  }

  template<typename Iter>
  bool push(Iter b, Iter e) {
    ChunkTy*& n = data.get();
    while (b != e) {
      if (!n)
	n = mkChunk();
      b = n->push(b, e);
      if (b != e) {
	worklist.push(static_cast<ChunkHeader*>(n));
	n = 0;
      }
    }
    return true;
  }

  template<typename Iter>
  void pushi(Iter b, Iter e)  {
    while (b != e) {
      ChunkTy* n = mkChunk();
      b = n->push(b,e);
      worklist.pushi(static_cast<ChunkHeader*>(n));
    }
  }

  std::pair<bool, value_type> pop()  {
    ChunkTy*& n = data.get();
    std::pair<bool, value_type> retval;
    //simple case, things in current chunk
    if (n && (retval = n->pop()).first)
      return retval;
    //empty chunk, trash it
    if (n)
      delChunk(n);
    //get a new chunk
    n = static_cast<ChunkTy*>(worklist.pop());
    if (n) {
      return n->pop();
    } else {
      return std::make_pair(false, value_type());
    }
  }
};
WLCOMPILECHECK(ChunkedAdaptor);

}
}
}
