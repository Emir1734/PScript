#pragma once

#include <string>

namespace pscript::bench {

inline std::string generated_functions_source(int count) {
    std::string source;
    source.reserve(static_cast<std::size_t>(count) * 48);
    for (int i = 0; i < count; ++i) {
        source += "func f" + std::to_string(i) + "(x):\n    return x + " + std::to_string(i) + "\n";
    }
    source += "print(f0(1))\n";
    return source;
}

inline const char kRuntimeArithmetic[] =
    "func acc(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(acc(8000))\n";

inline const char kRuntimeIntOps[] =
    "func imul(n):\n"
    "    total = 0\n"
    "    i = 1\n"
    "    while i < n:\n"
    "        total = total + i * 3\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(imul(6000))\n";

inline const char kRuntimeFloatOps[] =
    "func fsum(n):\n"
    "    total = 0.0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + 0.5\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(fsum(5000))\n";

inline const char kRuntimeNumLuIntCmp[] =
    "func cmp_loop(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if i < n - 1:\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(cmp_loop(25000))\n";

inline const char kRuntimeNumLuDivMod[] =
    "func divmod_loop(n):\n"
    "    total = 0\n"
    "    i = 1\n"
    "    while i < n:\n"
    "        total = total + (i / 3) + (i % 7)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(divmod_loop(12000))\n";

inline const char kRuntimeNumLuMixed[] =
    "func mixed(n):\n"
    "    total = 0.0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i + 0.25\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(mixed(8000))\n";

inline const char kRuntimeNumLuNested[] =
    "func nested_sum(n):\n"
    "    total = 0\n"
    "    outer = 0\n"
    "    while outer < n:\n"
    "        inner = 0\n"
    "        while inner < 32:\n"
    "            total = total + inner\n"
    "            inner = inner + 1\n"
    "        outer = outer + 1\n"
    "    return total\n"
    "print(nested_sum(800))\n";

inline const char kRuntimeNumLuShort[] =
    "func short_loop():\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < 8:\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(short_loop())\n";

inline const char kRuntimeNumLuConvert[] =
    "func convert(n):\n"
    "    total = 0.0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + float(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(convert(6000))\n";

inline const char kRuntimePerfLuLocalRw[] =
    "func local_rw(n):\n"
    "    x = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        x = x + 1\n"
    "        i = i + 1\n"
    "    return x\n"
    "print(local_rw(50000))\n";

inline const char kRuntimeLoop[] =
    "func loop(n):\n"
    "    s = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        s = s + 1\n"
    "        i = i + 1\n"
    "    return s\n"
    "print(loop(60000))\n";

inline const char kRuntimeSimpleCalls[] =
    "func inc(x):\n"
    "    return x + 1\n"
    "func hot(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + inc(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(hot(400000))\n";

inline const char kRuntimeRecursiveSum[] =
    "func rs(n):\n"
    "    if n <= 0:\n"
    "        return 0\n"
    "    return n + rs(n - 1)\n"
    "print(rs(8000))\n";

inline const char kRuntimeCalls[] =
    "func leaf(x):\n"
    "    return x + 1\n"
    "func mid(x):\n"
    "    return leaf(x) + leaf(x + 1)\n"
    "func top(x):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < x:\n"
    "        total = total + mid(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(top(2500))\n";

inline const char kRuntimeCallValueMono[] =
    "func f(x):\n"
    "    return x + 1\n"
    "func hot(n):\n"
    "    callee = f\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + callee(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(hot(400000))\n";

inline const char kRuntimeCallValuePoly[] =
    "func f(x):\n"
    "    return x + 1\n"
    "func g(x):\n"
    "    return x + 2\n"
    "func hot(n):\n"
    "    cf = f\n"
    "    cg = g\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if i % 2 == 0:\n"
    "            total = total + cf(i)\n"
    "        else:\n"
    "            total = total + cg(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(hot(80000))\n";

inline const char kRuntimeNestedCalls[] =
    "func a(x):\n"
    "    return x\n"
    "func b(x):\n"
    "    return a(x) + a(x + 1)\n"
    "func c(x):\n"
    "    return b(x) + b(x + 1)\n"
    "func d(x):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < x:\n"
    "        total = total + c(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(d(900))\n";

inline const char kRuntimeInlineTiny[] =
    "func bump(x):\n"
    "    return x + 1\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + bump(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(400000))\n";

inline const char kRuntimeInlineMedium[] =
    "func mix(x, y):\n"
    "    a = x + y\n"
    "    b = x - y\n"
    "    if a > b:\n"
    "        return a + 1\n"
    "    return b + 1\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + mix(i, i + 1)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(120000))\n";

inline const char kRuntimeInlineChain2[] =
    "func leaf(x):\n"
    "    return x + 1\n"
    "func mid(x):\n"
    "    return leaf(x) + leaf(x + 1)\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + mid(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(2500))\n";

inline const char kRuntimeInlineChain3[] =
    "func a(x):\n"
    "    return x + 1\n"
    "func b(x):\n"
    "    return a(x) + a(x + 1)\n"
    "func c(x):\n"
    "    return b(x) + b(x + 1)\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + c(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(900))\n";

inline const char kRuntimeInlineLargeCallee[] =
    "func heavy(x):\n"
    "    t = x\n"
    "    t = t + 1\n"
    "    t = t + 2\n"
    "    t = t + 3\n"
    "    t = t + 4\n"
    "    t = t + 5\n"
    "    t = t + 6\n"
    "    t = t + 7\n"
    "    t = t + 8\n"
    "    t = t + 9\n"
    "    t = t + 10\n"
    "    t = t + 11\n"
    "    t = t + 12\n"
    "    t = t + 13\n"
    "    t = t + 14\n"
    "    t = t + 15\n"
    "    t = t + 16\n"
    "    t = t + 17\n"
    "    t = t + 18\n"
    "    t = t + 19\n"
    "    t = t + 20\n"
    "    t = t + 21\n"
    "    t = t + 22\n"
    "    t = t + 23\n"
    "    t = t + 24\n"
    "    t = t + 25\n"
    "    return t\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + heavy(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(80000))\n";

inline const char kRuntimeInlineColdCall[] =
    "func helper(x):\n"
    "    return x + 1\n"
    "func bench(n):\n"
    "    total = helper(0)\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(400000))\n";

inline const char kRuntimeBranch[] =
    "func branch(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if i % 2 == 0:\n"
    "            total = total + i\n"
    "        else:\n"
    "            total = total - 1\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(branch(20000))\n";

inline const char kRuntimeFieldAccess[] =
    "struct Point:\n"
    "    x\n"
    "    y\n"
    "func read_fields(n):\n"
    "    p = Point()\n"
    "    p.x = 1\n"
    "    p.y = 2\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + p.x + p.y\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(read_fields(50000))\n";

inline const char kRuntimeListIndex[] =
    "func index_loop(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 256:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < items.length():\n"
    "            total = total + items[j]\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(index_loop(400))\n";

inline const char kRuntimeDictLookup[] =
    R"ps(
func dict_lookup(n):
    m = {}
    i = 0
    while i < 512:
        m["k" + str(i)] = i
        i = i + 1
    total = 0
    rep = 0
    while rep < n:
        j = 0
        while j < 512:
            total = total + m["k" + str(j)]
            j = j + 1
        rep = rep + 1
    return total
print(dict_lookup(200))
)ps";

inline const char kRuntimeDictLuStrHit[] =
    R"ps(
func bench(n):
    key = "hit"
    m = {}
    m[key] = 7
    total = 0
    i = 0
    while i < n:
        total = total + m[key]
        i = i + 1
    return total
print(bench(40000))
)ps";

inline const char kRuntimeDictLuStrMiss[] =
    R"ps(
func bench(n):
    m = {"exists": 1}
    hits = 0
    i = 0
    while i < n:
        if m.contains_key("missing"):
            hits = hits + 1
        i = i + 1
    return hits
print(bench(40000))
)ps";

inline const char kRuntimeDictLuIntHit[] =
    R"ps(
func bench(n):
    m = {}
    i = 0
    while i < 512:
        m[str(i)] = i
        i = i + 1
    key = str(128)
    total = 0
    rep = 0
    while rep < n:
        total = total + m[key]
        rep = rep + 1
    return total
print(bench(4000))
)ps";

inline const char kRuntimeDictLuIntMiss[] =
    R"ps(
func bench(n):
    m = {}
    i = 0
    while i < 512:
        m[str(i)] = i
        i = i + 1
    hits = 0
    rep = 0
    while rep < n:
        if m.contains_key(str(999)):
            hits = hits + 1
        rep = rep + 1
    return hits
print(bench(40000))
)ps";

inline const char kRuntimeDictLuSameKey[] =
    R"ps(
func bench(n):
    anchor = "anchor"
    m = {}
    m[anchor] = 3
    total = 0
    i = 0
    while i < n:
        total = total + m[anchor]
        i = i + 1
    return total
print(bench(50000))
)ps";

inline const char kRuntimeDictLuScatter[] =
    R"ps(
func bench(n):
    m = {}
    i = 0
    while i < 512:
        m["k" + str(i)] = i
        i = i + 1
    total = 0
    rep = 0
    while rep < n:
        j = 0
        while j < 512:
            idx = (rep * 17 + j * 31) % 512
            total = total + m["k" + str(idx)]
            j = j + 1
        rep = rep + 1
    return total
print(bench(100))
)ps";

inline const char kRuntimeDictLuLarge[] =
    R"ps(
func bench(n):
    m = {}
    i = 0
    while i < 4096:
        m["key" + str(i)] = i
        i = i + 1
    total = 0
    rep = 0
    while rep < n:
        j = 0
        while j < 4096:
            total = total + m["key" + str(j)]
            j = j + 1
        rep = rep + 1
    return total
print(bench(20))
)ps";

inline const char kRuntimeDictLuCollision[] =
    R"ps(
func bench(n):
    m = {}
    i = 0
    while i < 1024:
        m["collision_prefix_" + str(i)] = i
        i = i + 1
    total = 0
    rep = 0
    while rep < n:
        j = 0
        while j < 1024:
            total = total + m["collision_prefix_" + str(j)]
            j = j + 1
        rep = rep + 1
    return total
print(bench(80))
)ps";

inline const char kRuntimeDictLuInsertMix[] =
    R"ps(
func bench(n):
    m = {}
    total = 0
    i = 0
    while i < n:
        key = "k" + str(i % 256)
        m[key] = i
        total = total + m[key]
        i = i + 1
    return total
print(bench(8000))
)ps";

inline const char kRuntimeDictLuMixed[] =
    R"ps(
func bench(n):
    m = {"tag": 11}
    total = 0
    i = 0
    while i < n:
        total = total + m["tag"]
        total = total + i
        if m.contains_key("tag"):
            total = total + 1
        i = i + 1
    return total
print(bench(6000))
)ps";

inline const char kRuntimeSetLookup[] =
    "func set_lookup(n):\n"
    "    s = {0}\n"
    "    s.clear()\n"
    "    i = 0\n"
    "    while i < 256:\n"
    "        s.add(i)\n"
    "        i = i + 1\n"
    "    hits = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < 256:\n"
    "            if j in s:\n"
    "                hits = hits + 1\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return hits\n"
    "print(set_lookup(300))\n";

inline const char kRuntimeListIterate[] =
    "func sum_list(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 512:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        for x in items:\n"
    "            total = total + x\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(sum_list(120))\n";

inline const char kRuntimeRangeIterate[] =
    "func sum_range(n):\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        for x in range(512):\n"
    "            total = total + x\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(sum_range(120))\n";

inline const char kRuntimeListWrite[] =
    "func write_loop(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 256:\n"
    "        items.append(0)\n"
    "        i = i + 1\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < items.length():\n"
    "            items[j] = j\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return items[0] + items[255]\n"
    "print(write_loop(400))\n";

inline const char kRuntimeTupleIndex[] =
    "func tuple_loop(n):\n"
    "    t = (1, 2, 3, 4, 5, 6, 7, 8)\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        i = 0\n"
    "        while i < 8:\n"
    "            total = total + t[i]\n"
    "            i = i + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(tuple_loop(400))\n";

inline const char kRuntimeBytesIndex[] =
    "import encoding\n"
    "func bytes_loop(n):\n"
    "    b = encoding.encode(\"abcdefghijklmnopqrstuvwxyz0123456789\")\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < b.length():\n"
    "            total = total + b[j]\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bytes_loop(800))\n";

inline const char kRuntimeNestedCollection[] =
    "func nested(n):\n"
    "    grid = []\n"
    "    i = 0\n"
    "    while i < 16:\n"
    "        row = []\n"
    "        j = 0\n"
    "        while j < 16:\n"
    "            row.append(i * 16 + j)\n"
    "            j = j + 1\n"
    "        grid.append(row)\n"
    "        i = i + 1\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        r = 0\n"
    "        while r < grid.length():\n"
    "            c = 0\n"
    "            while c < grid[r].length():\n"
    "                total = total + grid[r][c]\n"
    "                c = c + 1\n"
    "            r = r + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(nested(30))\n";

inline const char kRuntimeCollectionClosure[] =
    "func make_sum(items):\n"
    "    func add_all():\n"
    "        total = 0\n"
    "        for x in items:\n"
    "            total = total + x\n"
    "        return total\n"
    "    return add_all\n"
    "func bench(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 128:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    f = make_sum(items)\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        total = total + f()\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bench(400))\n";

inline const char kRuntimeClosureCall[] =
    "func make_adder(base):\n"
    "    func add(x):\n"
    "        return base + x\n"
    "    return add\n"
    "func bench(n):\n"
    "    f = make_adder(100)\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + f(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(25000))\n";

inline const char kRuntimeClosureMutable[] =
    "func make_counter():\n"
    "    n = 0\n"
    "    func inc():\n"
    "        n = n + 1\n"
    "        return n\n"
    "    return inc\n"
    "func bench(k):\n"
    "    f = make_counter()\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < k:\n"
    "        total = total + f()\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(25000))\n";

inline const char kRuntimeClosureNested[] =
    "func outer(x):\n"
    "    func mid(y):\n"
    "        func inner(z):\n"
    "            return x + y + z\n"
    "        return inner\n"
    "    return mid\n"
    "func bench(n):\n"
    "    f = outer(10)\n"
    "    g = f(20)\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + g(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(25000))\n";

inline const char kRuntimeClosureCreate[] =
    "func make_adder(base):\n"
    "    func add(x):\n"
    "        return base + x\n"
    "    return add\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        f = make_adder(i)\n"
    "        total = total + f(1)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(8000))\n";

inline const char kRuntimeClosurePoly[] =
    "func mk(base):\n"
    "    func f(x):\n"
    "        return base + x\n"
    "    return f\n"
    "func bench(n):\n"
    "    fa = mk(1)\n"
    "    fb = mk(1000)\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if i % 2 == 0:\n"
    "            total = total + fa(i)\n"
    "        else:\n"
    "            total = total + fb(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(25000))\n";

inline const char kRuntimeClosureShared[] =
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
    "print(shared(10000))\n";

inline const char kRuntimeClosureRecursive[] =
    "func make_sum():\n"
    "    func sum(n):\n"
    "        if n <= 0:\n"
    "            return 0\n"
    "        return n + sum(n - 1)\n"
    "    return sum\n"
    "f = make_sum()\n"
    "print(f(200))\n";

inline const char kRuntimeCollections[] =
    "func coll(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    return items.length()\n"
    "print(coll(3000))\n";

inline const char kRuntimeCollectionAllocation[] =
    "func allocate(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        items = [i, i + 1, i + 2, i + 3]\n"
    "        total = total + items[0] + items[3]\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(allocate(3000))\n";

inline const char kRuntimeString[] =
    "func swork(n):\n"
    "    s = \"\"\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        s = s + \"x\"\n"
    "        i = i + 1\n"
    "    return s.length()\n"
    "print(swork(250))\n";

inline const char kRuntimeStringEq[] =
    R"ps(
func seq(n):
    hits = 0
    i = 0
    while i < n:
        if "needle" == "needle":
            hits = hits + 1
        if "abc" != "xyz":
            hits = hits + 1
        i = i + 1
    return hits
print(seq(8000))
)ps";

inline const char kRuntimeStringEqSameRef[] =
    "func seq(n):\n"
    "    needle = \"needle\"\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if needle == needle:\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(8000))\n";

inline const char kRuntimeStringEqDiffObject[] =
    "func seq(n):\n"
    "    left = \"same-contents\"\n"
    "    right = \"same-contents\"\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if left == right:\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(8000))\n";

inline const char kRuntimeStringEqDiffLen[] =
    "func seq(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if \"ab\" != \"abc\":\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(8000))\n";

inline const char kRuntimeStringEqSameLenDiff[] =
    "func seq(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if \"alpha\" != \"alphb\":\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(8000))\n";

inline const char kRuntimeStringEqLong[] =
    "func seq(n):\n"
    "    left = \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\n"
    "    right = \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if left == right:\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(2000))\n";

inline const char kRuntimeStringEqShort[] =
    "func seq(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if \"a\" == \"b\":\n"
    "            hits = hits + 1\n"
    "        if \"x\" != \"y\":\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(12000))\n";

inline const char kRuntimeStringEqMixed[] =
    "func seq(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if \"tag\" == \"tag\":\n"
    "            hits = hits + 1\n"
    "        hits = hits + i\n"
    "        if \"no\" != \"match\":\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(4000))\n";

inline const char kRuntimeStringSearch[] =
    "func sfind(n):\n"
    "    hits = 0\n"
    "    text = \"the quick brown fox jumps over the lazy dog\"\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if text.contains(\"fox\"):\n"
    "            hits = hits + 1\n"
    "        if text.starts_with(\"the\"):\n"
    "            hits = hits + 1\n"
    "        if text.ends_with(\"dog\"):\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(sfind(4000))\n";

inline const char kRuntimeStringMethods[] =
    "func sm(n):\n"
    "    text = \"Hello World\"\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        hits = hits + text.length()\n"
    "        if text.contains(\"lo\"):\n"
    "            hits = hits + 1\n"
    "        hits = hits + text.replace(\"World\", \"PScript\").length()\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(sm(500))\n";

inline const char kRuntimeStringHeavyLoop[] =
    "func heavy(n):\n"
    "    total = \"\"\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + str(i) + \":\"\n"
    "        i = i + 1\n"
    "    return total.length()\n"
    "print(heavy(120))\n";

inline const char kRuntimeStringIndex[] =
    "func str_index(n):\n"
    "    text = \"abcdefghij\"\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + text[0] + text[9]\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(str_index(5000))\n";

inline const char kRuntimeStringUtf8[] =
    "func utf8_work(n):\n"
    "    text = \"\xC3\xA7\xC4\x9F\xC4\xB1\"\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + text.length()\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(utf8_work(2000))\n";

inline const char kRuntimeMixedStringCollection[] =
    "func mix(n):\n"
    "    rows = []\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        rows.append(\"k\" + str(i))\n"
    "        i = i + 1\n"
    "    hits = 0\n"
    "    j = 0\n"
    "    while j < rows.length():\n"
    "        if rows[j].starts_with(\"k\"):\n"
    "            hits = hits + 1\n"
    "        j = j + 1\n"
    "    return hits\n"
    "print(mix(400))\n";

inline const char kRuntimeException[] =
    "func boom(n):\n"
    "    if n == 0:\n"
    "        throw RuntimeException(\"x\", null)\n"
    "    return n\n"
    "total = 0\n"
    "i = 0\n"
    "while i < 200:\n"
    "    try:\n"
    "        total = total + boom(1)\n"
    "    catch RuntimeException e:\n"
    "        total = total + 1\n"
    "    i = i + 1\n"
    "print(total)\n";

inline const char kRuntimeDefer[] =
    "func work(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        defer total = total + 1\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(work(800))\n";

inline const char kRuntimeAsync[] =
    "async func worker(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "async func main():\n"
    "    a = worker(500)\n"
    "    b = worker(500)\n"
    "    print(await a + await b)\n";

inline const char kJitHotKernel[] =
    "func kernel(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i * 2\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(kernel(50000))\n";

inline const char kJitAmortizedKernel[] =
    "func kernel(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i * 2\n"
    "        i = i + 1\n"
    "    return total\n"
    "acc = 0\n"
    "rep = 0\n"
    "while rep < 5:\n"
    "    acc = acc + kernel(4000)\n"
    "    rep = rep + 1\n"
    "print(acc)\n";

inline const char kStdlibMath[] =
    "import math\n"
    "total = 0.0\n"
    "i = 0\n"
    "while i < 20000:\n"
    "    total = total + math.sqrt(i + 1)\n"
    "    i = i + 1\n"
    "print(total)\n";

inline const char kStdlibJson[] =
    "import json\n"
    "payload = \"{\\\"items\\\":[\"\n"
    "i = 0\n"
    "while i < 500:\n"
    "    if i > 0:\n"
    "        payload = payload + \",\"\n"
    "    payload = payload + str(i)\n"
    "    i = i + 1\n"
    "payload = payload + \"]}\"\n"
    "doc = json.decode(payload)\n"
    "print(doc[\"items\"].length())\n";

inline const char kStdlibJsonEncode[] =
    "import json\n"
    "items = []\n"
    "i = 0\n"
    "while i < 200:\n"
    "    items.append(i)\n"
    "    i = i + 1\n"
    "doc = {\"items\": items}\n"
    "encoded = json.encode(doc)\n"
    "print(encoded.length())\n";

inline const char kStdlibJsonRoundtrip[] =
    "import json\n"
    "doc = {\"name\": \"pscript\", \"values\": [1, 2, 3], \"ok\": true}\n"
    "encoded = json.encode(doc)\n"
    "decoded = json.decode(encoded)\n"
    "print(decoded[\"values\"].length())\n";

inline const char kStdlibJsonNested[] =
    "import json\n"
    "payload = \"{\\\"a\\\":{\\\"b\\\":{\\\"c\\\":[1,2,3]}}}\"\n"
    "doc = json.decode(payload)\n"
    "print(doc[\"a\"][\"b\"][\"c\"].length())\n";

inline const char kStdlibJsonPipeline[] =
    "import json\n"
    "payload = \"{\\\"items\\\":[\"\n"
    "i = 0\n"
    "while i < 500:\n"
    "    if i > 0:\n"
    "        payload = payload + \",\"\n"
    "    payload = payload + str(i)\n"
    "    i = i + 1\n"
    "payload = payload + \"]}\"\n"
    "doc = json.decode(payload)\n"
    "sum = 0\n"
    "for v in doc[\"items\"]:\n"
    "    sum = sum + v\n"
    "encoded = json.encode(doc)\n"
    "print(sum + encoded.length())\n";

inline const char kRealMixedRuntime[] =
    "import json\n"
    "payload = \"{\\\"items\\\":[\"\n"
    "i = 0\n"
    "while i < 500:\n"
    "    if i > 0:\n"
    "        payload = payload + \",\"\n"
    "    payload = payload + str(i)\n"
    "    i = i + 1\n"
    "payload = payload + \"]}\"\n"
    "doc = json.decode(payload)\n"
    "sum = 0\n"
    "for v in doc[\"items\"]:\n"
    "    sum = sum + v\n"
    "encoded = json.encode(doc)\n"
    "doc2 = json.decode(encoded)\n"
    "print(sum + encoded.length() + doc2[\"items\"].length())\n";

inline const char kStdlibArchiveZipSmall[] =
    "import archive\n"
    "import encoding\n"
    "import fs\n"
    "func round(n):\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        path = \"bench_perf_pack.zip\"\n"
    "        a = archive.create(path)\n"
    "        a.write(\"payload.txt\", encoding.encode(\"bench-perf-data\"))\n"
    "        a.close()\n"
    "        r = archive.open(path)\n"
    "        total = total + encoding.decode(r.read(\"payload.txt\")).length()\n"
    "        r.close()\n"
    "        fs.remove(path)\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(round(25))\n";

inline const char kStdlibCsv[] =
    "import csv\n"
    "text = \"\"\n"
    "i = 0\n"
    "while i < 800:\n"
    "    text = text + str(i) + \",\" + str(i * 2) + \"\\n\"\n"
    "    i = i + 1\n"
    "rows = csv.parse(text)\n"
    "print(rows.length())\n";

inline const char kStdlibUrl[] =
    "import url\n"
    "u = url.parse(\"https://example.com/path?q=1\")\n"
    "print(u[\"host\"])\n";

inline const char kStdlibUuid[] =
    "import uuid\n"
    "i = 0\n"
    "while i < 2000:\n"
    "    uuid.v4()\n"
    "    i = i + 1\n"
    "print(i)\n";

inline const char kStdlibXml[] =
    "import xml\n"
    "doc = xml.parse(\"<root><item>1</item><item>2</item></root>\")\n"
    "print(doc[\"children\"].length())\n";

inline const char kStdlibCrypto[] =
    "import crypto\n"
    "data = \"benchmark-payload\"\n"
    "i = 0\n"
    "while i < 400:\n"
    "    crypto.sha256(data)\n"
    "    i = i + 1\n"
    "print(i)\n";

inline const char kRealJsonPipeline[] =
    "import json\n"
    "import fs\n"
    "fs.write(\"bench_data.json\", \"{\\\"values\\\":[1,2,3,4,5]}\")\n"
    "raw = fs.read(\"bench_data.json\")\n"
    "doc = json.decode(raw)\n"
    "total = 0\n"
    "for v in doc[\"values\"]:\n"
    "    total = total + v\n"
    "print(total)\n";

inline const char kRealCsvAggregate[] =
    "import csv\n"
    "import fs\n"
    "lines = \"\"\n"
    "i = 0\n"
    "while i < 1000:\n"
    "    lines = lines + str(i) + \",\" + str(i * 3) + \"\\n\"\n"
    "    i = i + 1\n"
    "fs.write(\"bench.csv\", lines)\n"
    "rows = csv.parse(fs.read(\"bench.csv\"))\n"
    "total = 0\n"
    "for row in rows:\n"
    "    total = total + int(row[1])\n"
    "print(total)\n";

inline const char kRealSqlite[] =
    "import sql\n"
    "db = sql.open(\":memory:\")\n"
    "db.execute(\"CREATE TABLE t (id INTEGER, v INTEGER)\")\n"
    "i = 0\n"
    "while i < 500:\n"
    "    db.execute(\"INSERT INTO t (id, v) VALUES (?, ?)\", [i, i * 2])\n"
    "    i = i + 1\n"
    "row = db.query_one(\"SELECT SUM(v) AS total FROM t\", [])\n"
    "print(row[\"total\"])\n";

}  // namespace pscript::bench
