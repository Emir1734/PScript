#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"
#include <iostream>
#include <sstream>
using namespace pscript;
int main() {
  const char* source =
    "func shared(n):\n"
    "    x = 0\n"
    "    func inc():\n"
    "        x = x + 1\n"
    "        return x\n"
    "    func read():\n"
    "        return x\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        inc()\n"
    "        total = total + read()\n"
    "        i = i + 1\n"
    "    return total\n"
    "func outer(x):\n"
    "    func mid(y):\n"
    "        func inner(z):\n"
    "            return x + y + z\n"
    "        return inner\n"
    "    return mid\n"
    "print(shared(200))\n"
    "f = outer(10)\n"
    "g = f(20)\n"
    "print(g(3))\n"
    "func make_recursive():\n"
    "    func sum(n):\n"
    "        if n <= 0:\n"
    "            return 0\n"
    "        return n + sum(n - 1)\n"
    "    return sum\n"
    "r = make_recursive()\n"
    "print(r(20))\n";
  CompileOptions copt; copt.source_file="<t>";
  Bytecode bc = compile(parse(source), copt);
  std::ostringstream out; Runtime rt(out); Vm vm(std::move(bc), rt);
  jit::JitTieringConfig cfg; cfg.baseline_invocation_threshold=1;
  cfg.optimizing_invocation_threshold=1; cfg.require_profile_for_optimizing=false;
  vm.set_jit_enabled(true); vm.set_jit_config(cfg);
  std::cout << "run\n" << std::flush;
  vm.run();
  std::cout << "out=" << out.str() << "ok\n";
  return 0;
}
