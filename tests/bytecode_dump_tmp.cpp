#include "compiler/compiler.h"
#include "parser/parser.h"
#include <iostream>
using namespace pscript;
int main() {
  const char* src =
    "func outer(a, b, c):\n"
    "    func inner(z):\n"
    "        return a + b + c + z\n"
    "    return inner\n"
    "func main(n):\n"
    "    f = outer(1, 2, 3)\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + f(i)\n"
    "        i = i + 1\n"
    "    return total\n";
  CompileOptions o; o.source_file="<t>";
  Bytecode bc = compile(parse(src), o);
  for (uint32_t fi = 0; fi < bc.functions.size(); ++fi) {
    auto& f = bc.functions[fi];
    std::cout << fi << " " << f.name << " caps=" << f.captures.size() << " entry=" << f.entry << "\n";
  }
  for (uint32_t fi = 0; fi < bc.functions.size(); ++fi) {
    auto& fn = bc.functions[fi];
    uint32_t end = bc.instructions.size();
    for (uint32_t j = fi+1; j < bc.functions.size(); ++j)
      if (bc.functions[j].entry > fn.entry && bc.functions[j].entry < end) end = bc.functions[j].entry;
    std::cout << "--- " << fn.name << " ---\n";
    for (uint32_t ip = fn.entry; ip < end; ++ip) {
      auto& in = bc.instructions[ip];
      const char* op = "?";
      switch (in.opcode) {
        case Opcode::Call: op="Call"; break;
        case Opcode::CallValue: op="CallValue"; break;
        case Opcode::MakeClosure: op="MakeClosure"; break;
        case Opcode::LoadFunction: op="LoadFunction"; break;
        case Opcode::StoreLocal: op="StoreLocal"; break;
        case Opcode::LoadLocal: op="LoadLocal"; break;
        case Opcode::Return: op="Return"; break;
        default: break;
      }
      if (strcmp(op,"?")) std::cout << ip << " " << op << " a=" << in.a << " b=" << in.b << " c=" << in.c << "\n";
    }
  }
}
