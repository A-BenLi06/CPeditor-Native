// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpn/checker.hpp"
#include "cpn/digest.hpp"
#include "cpn/text.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void checkers() {
    using M = cpn::ComparisonMode; using V = cpn::ComparisonVerdict;
    const std::vector<std::tuple<std::string,std::string,M,V>> cases{
        {"1\r\n2\r3", "1\n2\n3", M::cp_editor_strict,V::accepted},
        {"1\n", "1", M::cp_editor_strict,V::wrong_answer},
        {"1 \t\n\n \t\n", "1", M::cp_editor_ignore_trailing_spaces,V::accepted},
        {" 1", "1", M::cp_editor_ignore_trailing_spaces,V::wrong_answer},
        {"1  2", "1 2", M::cp_editor_ignore_trailing_spaces,V::wrong_answer},
        {"1\xc2\xa0\n\xe3\x80\x80\n", "1",M::cp_editor_ignore_trailing_spaces,V::accepted},
        {"1\xe2\x80\x8b", "1",M::cp_editor_ignore_trailing_spaces,V::wrong_answer},
        {"", " \t\n",M::cp_editor_ignore_trailing_spaces,V::accepted},
        {std::string("a\0b",3),"ab",M::cp_editor_strict,V::wrong_answer},
        {std::string("a\0b",3),std::string("a\0b",3),M::cp_editor_strict,V::accepted},
        {"a\r\n","a\n",M::byte_exact,V::wrong_answer},
        {std::string("\xff",1),std::string("\xff",1),M::byte_exact,V::accepted},
        {std::string("\xff",1),"",M::cp_editor_strict,V::encoding_error},
        {"",std::string("\xc0\x80",2),M::cp_editor_strict,V::encoding_error}
    };
    for (const auto& [out,ans,mode,verdict] : cases)
        expect(cpn::compare_outputs(out,ans,mode).verdict==verdict,"checker compatibility");
}
void positions() {
    const std::string text="a\xe4\xb8\xad\xf0\x9f\x98\x80" "e\xcc\x81\r\nb\rc\n";
    const auto index=cpn::TextIndex::make(text);
    expect(index.has_value(),"valid Unicode index");
    expect(index->line_count()==4,"mixed newline line count");
    expect(index->byte_offset({0,4},cpn::PositionEncoding::utf16)==8,"UTF-16 surrogate pair");
    expect(!index->byte_offset({0,3},cpn::PositionEncoding::utf16),"reject surrogate split");
    expect(!index->position(2,cpn::PositionEncoding::utf16),"reject UTF-8 split");
    expect(index->byte_offset({0,6},cpn::PositionEncoding::utf16)==11,"combining code units");
    expect(!index->position(12,cpn::PositionEncoding::utf16),"reject CRLF interior");
    expect(index->position(13,cpn::PositionEncoding::utf16)==cpn::TextPosition{1,0},"CRLF next line");
    expect(!index->byte_offset({9,0},cpn::PositionEncoding::utf8),"reject missing line");
    expect(!index->byte_offset({0,99},cpn::PositionEncoding::utf8),"reject past line");
    expect(cpn::TextIndex::make("")->line_count()==1,"empty document");
    for(const auto& invalid:std::vector<std::string>{"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82"})
        expect(!cpn::TextIndex::make(invalid),"reject invalid UTF-8");
    for(std::size_t offset=0;offset<=text.size();++offset)
        for(const auto encoding:{cpn::PositionEncoding::utf8,cpn::PositionEncoding::utf16,cpn::PositionEncoding::utf32})
            if(const auto position=index->position(offset,encoding))
                expect(index->byte_offset(*position,encoding)==offset,"position round trip");
}
void digests() {
    expect(cpn::sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
    expect(cpn::sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
    expect(cpn::sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA256 blocks");
}
}
int main() {
    try { checkers();positions();digests();std::cout<<"domain tests passed\n";return 0; }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
