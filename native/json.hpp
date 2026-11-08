// MiniLyrics JSON transport codec. GPL-3.0-or-later.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdlib>
#include <cmath>
#include <cstdio>
struct Json {
 enum Kind {Null,Number,Bool,String,Array,Object} kind=Null;
 double number=0;bool boolean=false;std::string text;
 std::vector<Json> array;std::map<std::string,Json> object;
 Json()=default;Json(double n):kind(Number),number(std::isfinite(n)?n:0){}
 Json(int n):Json(double(n)){} Json(bool b):kind(Bool),boolean(b){}
 Json(const char*s):kind(String),text(s){} Json(std::string s):kind(String),text(std::move(s)){}
 static Json obj(){Json j;j.kind=Object;return j;}
 const Json& operator[](const std::string& key)const {static const Json empty;auto i=object.find(key);return i==object.end()?empty:i->second;}
 Json& operator[](const std::string& key){kind=Object;return object[key];}
 std::string s(const std::string& fallback="")const{return kind==String?text:fallback;}
 double n(double fallback=0)const{return kind==Number&&std::isfinite(number)?number:fallback;}
 bool b(bool fallback=false)const{return kind==Bool?boolean:fallback;}
 static std::string quote(const std::string&s){std::string out="\"";char hex[7];for(unsigned char c:s){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<32){std::snprintf(hex,sizeof(hex),"\\u%04x",c);out+=hex;}else out+=char(c);}}return out+'"';}
 std::string dump()const {switch(kind){case Null:return "null";case Bool:return boolean?"true":"false";case Number:{char b[40];std::snprintf(b,sizeof(b),"%.15g",number);return b;}case String:return quote(text);case Array:{std::string s="[";for(auto&v:array){if(s.size()>1)s+=',';s+=v.dump();}return s+']';}case Object:{std::string s="{";for(auto&v:object){if(s.size()>1)s+=',';s+=quote(v.first)+':'+v.second.dump();}return s+'}';}}return "null";}
};
class JsonParser {
 const std::string&s;size_t p=0;
 [[noreturn]] void fail(){throw std::runtime_error("invalid JSON");}
 void ws(){while(p<s.size()&&(s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t'))p++;}
 bool take(char c){ws();if(p<s.size()&&s[p]==c){p++;return true;}return false;}
 unsigned hex(){unsigned v=0;for(int i=0;i<4;i++){if(p>=s.size())fail();char c=s[p++];unsigned n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;if(n>15)fail();v=(v<<4)|n;}return v;}
 void utf8(std::string&out,unsigned c){if(c<128)out+=char(c);else if(c<2048){out+=char(192|(c>>6));out+=char(128|(c&63));}else if(c<65536){out+=char(224|(c>>12));out+=char(128|((c>>6)&63));out+=char(128|(c&63));}else{out+=char(240|(c>>18));out+=char(128|((c>>12)&63));out+=char(128|((c>>6)&63));out+=char(128|(c&63));}}
 std::string str(){if(!take('"'))fail();std::string out;while(p<s.size()){unsigned char c=s[p++];if(c=='"')return out;if(c<32)fail();if(c!='\\'){out+=char(c);continue;}if(p>=s.size())fail();char e=s[p++];switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'u':{unsigned cp=hex();if(cp>=0xd800&&cp<=0xdbff){if(p+2>s.size()||s[p++]!='\\'||s[p++]!='u')fail();unsigned lo=hex();if(lo<0xdc00||lo>0xdfff)fail();cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);}else if(cp>=0xdc00&&cp<=0xdfff)fail();utf8(out,cp);break;}default:fail();}}fail();}
 Json value(int depth){if(depth>64)fail();ws();if(p>=s.size())fail();if(s[p]=='"')return Json(str());if(take('{')){Json j=Json::obj();if(take('}'))return j;do{auto key=str();if(!take(':'))fail();j.object[key]=value(depth+1);}while(take(','));if(!take('}'))fail();return j;}if(take('[')){Json j;j.kind=Json::Array;if(take(']'))return j;do{j.array.push_back(value(depth+1));}while(take(','));if(!take(']'))fail();return j;}for(auto token:{"true","false","null"}){size_t len=std::char_traits<char>::length(token);if(s.compare(p,len,token)==0){p+=len;return token[0]=='n'?Json():Json(token[0]=='t');}}
 size_t begin=p;if(s[p]=='-')p++;if(p>=s.size())fail();if(s[p]=='0')p++;else {if(s[p]<'1'||s[p]>'9')fail();while(p<s.size()&&s[p]>='0'&&s[p]<='9')p++;}if(p<s.size()&&s[p]=='.'){p++;size_t b=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')p++;if(p==b)fail();}if(p<s.size()&&(s[p]=='e'||s[p]=='E')){p++;if(p<s.size()&&(s[p]=='+'||s[p]=='-'))p++;size_t b=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')p++;if(p==b)fail();}double n=std::strtod(s.c_str()+begin,nullptr);if(!std::isfinite(n))fail();return Json(n);}
 public:explicit JsonParser(const std::string& input):s(input){} Json parse(){auto j=value(0);ws();if(p!=s.size())fail();return j;}
};
