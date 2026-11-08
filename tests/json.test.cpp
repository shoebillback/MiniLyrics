#include "../native/json.hpp"
#include <cassert>
#include <iostream>
int main(){
 const std::string unicode=u8"中文歌词 🎵";
 auto message=JsonParser("{\"type\":\"lyrics\",\"lines\":[{\"text\":\"\\u4e2d\\u6587\\u6b4c\\u8bcd \\ud83c\\udfb5\",\"time\":123.5}],\"valid\":true}").parse();
 assert(message["lines"].array[0]["text"].s()==unicode);
 assert(message["lines"].array[0]["time"].n()==123.5);
 assert(JsonParser(message.dump()).parse()["valid"].b());
 for(auto invalid:{"{", "[1,]", "true false", "01", "1e", "1e999", "\"\\ud800\"", "\"\\udc00\"", "\"line\nfeed\""}){
  bool rejected=false;try{JsonParser(invalid).parse();}catch(...){rejected=true;}assert(rejected);
 }
 auto quoted=Json::quote(std::string("line\n\"\\\t")+unicode);
 assert(JsonParser(quoted).parse().s()==std::string("line\n\"\\\t")+unicode);
 bool deepRejected=false;try{JsonParser(std::string(70,'[')+"0"+std::string(70,']')).parse();}catch(...){deepRejected=true;}assert(deepRejected);
 std::cout<<"PASS: JSON transport, Unicode surrogate pairs, escaping, invalid input and nesting limit\n";
}
