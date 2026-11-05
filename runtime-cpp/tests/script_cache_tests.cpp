#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "v8trace-script-intern-cache.h"
#include "v8trace-json.h"
#include "v8trace-sha256.h"

struct Counted {
  static inline int live = 0;
  int value;
  explicit Counted(int item):value(item){++live;}
  Counted(const Counted& other):value(other.value){++live;}
  ~Counted(){--live;}
  bool operator<(const Counted& other)const{return value<other.value;}
};

int main(int argc,char** argv){
  assert(argc==2);
  using Key=std::pair<std::string,std::size_t>;
  v8trace_script_intern::CacheTraits::Index<Key> index;
  std::size_t iid=0,previous=0;
  assert(!index.LookUpOrInsert(&iid,Key{"000",9})&&iid==1);
  assert(index.LookUpOrInsert(&iid,Key{"000",9})&&iid==1);
  previous=iid;
  for(int i=0;i<4096;++i){
    Key key{"x"+std::to_string(i),static_cast<std::size_t>(i)};
    assert(!index.LookUpOrInsert(&iid,key)&&iid>previous);previous=iid;
    assert(index.LookUpOrInsert(&iid,key)&&iid==previous);
  }
  assert(!index.LookUpOrInsert(&iid,Key{"000",9})&&iid>previous);
  previous=iid;
  assert(!index.LookUpOrInsert(&iid,Key{"000",9007199254740993ULL})&&iid>previous);
  {v8trace_script_intern::CacheTraits::Index<Counted> bounded;
    for(int i=0;i<4096;++i){Counted key(i);assert(!bounded.LookUpOrInsert(&iid,key));assert(Counted::live<=257);}
  }
  assert(Counted::live==0);
  v8trace_script_intern::CacheTraits::Index<Key> hot;
  std::size_t hot_iid=0;
  assert(!hot.LookUpOrInsert(&hot_iid,Key{"hot",3}));
  for(int i=0;i<4096;++i){
    assert(!hot.LookUpOrInsert(&iid,Key{"cold"+std::to_string(i),static_cast<std::size_t>(i)}));
    assert(hot.LookUpOrInsert(&iid,Key{"hot",3})&&iid==hot_iid);
  }
  v8trace_script_intern::CacheTraits::Index<Key> reset;
  assert(!reset.LookUpOrInsert(&iid,Key{"different",1})&&iid==1);
  v8trace_sha256::Hash empty;
  assert(empty.Finish()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  v8trace_sha256::Hash abc;for(unsigned char byte:std::string("abc"))abc.Add(byte);
  assert(abc.Finish()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::vector<std::uint16_t> units;for(unsigned i=0;i<65536;++i)units.push_back(static_cast<std::uint16_t>(i));
  std::size_t position=0;std::ostringstream out;
  v8trace_json::WriteUtf16(out,units.size(),[&]{return units.at(position++);});
  const auto literal=out.str();v8trace_sha256::Hash hash;
  for(unsigned char byte:literal){assert(byte<128);hash.Add(byte);}
  const auto digest=hash.Finish();
  assert(literal.size()==392745 && digest=="5fe3133df7b866fb46a0cc21fb52329788bf09d312dd9d6c7deec6444a06fa9e");
  std::ofstream file(argv[1],std::ios::binary);file.write(literal.data(),literal.size());file.close();assert(file);
  std::cout<<"{\"status\":\"passed\",\"boundedKeys\":256,\"monotonicEvictions\":4096,\"wideLengthDistinct\":true,\"resetScope\":true,\"sourceCodeUnits\":65536,\"literalBytes\":"<<literal.size()<<",\"literalSha256\":\""<<digest<<"\",\"nativeBrowserAccepted\":false}"<<std::endl;
}
