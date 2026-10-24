using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Text.Json;

static long RunPscriptLoops()
{
    const long n = 60_000;
    long s = 0;
    for (long i = 0; i < n; ++i)
    {
        s += 1;
    }
    return s;
}

static long RunPscriptNumLuDivmod()
{
    const long n = 12_000;
    long total = 0;
    for (long i = 1; i < n; ++i)
    {
        total += (i / 3) + (i % 7);
    }
    return total;
}

static long RunPscriptClosureCall()
{
    const long n = 25_000;
    const long baseVal = 100;
    Func<long, long> add = x => baseVal + x;
    long total = 0;
    for (long i = 0; i < n; ++i)
    {
        total += add(i);
    }
    return total;
}

static long RunPscriptClosureNested()
{
    const long n = 25_000;
    Func<long, Func<long, Func<long, long>>> outer = x => y => z => x + y + z;
    var g = outer(10)(20);
    long total = 0;
    for (long i = 0; i < n; ++i)
    {
        total += g(i);
    }
    return total;
}

static long RunPscriptClosureRecursive()
{
    return SumRecursive(200);
}

static long SumRecursive(long n)
{
    if (n <= 0)
    {
        return 0;
    }
    return n + SumRecursive(n - 1);
}

static long RunPscriptClosureRecursiveIter()
{
    long acc = 0;
    for (long n = 200; n > 0; --n)
    {
        acc += n;
    }
    return acc;
}

static long RunIntLoop(ulong n)
{
    long sum = 0;
    for (ulong i = 0; i < n; ++i)
    {
        sum += (long)i;
    }
    return sum;
}

static long RunArithmetic(ulong n) => RunIntLoop(n);

static long RunStringEq(ulong repetitions)
{
    long hits = 0;
    const string needle = "needle";
    const string abc = "abc";
    const string xyz = "xyz";
    for (ulong i = 0; i < repetitions; ++i)
    {
        if (needle == "needle")
        {
            ++hits;
        }
        if (abc != xyz)
        {
            ++hits;
        }
    }
    return hits;
}

static long RunStringConcat(ulong repetitions)
{
    var s = "";
    for (ulong i = 0; i < repetitions; ++i)
    {
        s += "x";
    }
    return s.Length;
}

static long RunListIndex(ulong repetitions)
{
    long total = 0;
    var items = new long[256];
    for (int i = 0; i < items.Length; ++i)
    {
        items[i] = i;
    }
    for (ulong rep = 0; rep < repetitions; ++rep)
    {
        for (int i = 0; i < items.Length; ++i)
        {
            total += items[i];
        }
    }
    return total;
}

static long Inc(long x) => x + 1;

static long RunDirectIncCall(ulong n)
{
    long total = 0;
    for (ulong i = 0; i < n; ++i)
    {
        total += Inc((long)i);
    }
    return total;
}

static long Leaf(long x) => x;

static long Mid(long x) => Leaf(x) + Leaf(x + 1);

static long RunNestedCall(ulong n)
{
    long total = 0;
    for (ulong i = 0; i < n; ++i)
    {
        total += Mid((long)i);
    }
    return total;
}

static long RunDictLookup(ulong repetitions)
{
    var map = new Dictionary<string, long>(512);
    for (int i = 0; i < 512; ++i)
    {
        map["k" + i] = i;
    }
    long total = 0;
    for (ulong rep = 0; rep < repetitions; ++rep)
    {
        for (int j = 0; j < 512; ++j)
        {
            total += map["k" + j];
        }
    }
    return total;
}

static string JsonBenchmarkPayload()
{
    var sb = new StringBuilder();
    sb.Append("{\"items\":[");
    for (int i = 0; i < 500; ++i)
    {
        if (i > 0)
        {
            sb.Append(',');
        }
        sb.Append(i);
    }
    sb.Append("]}");
    return sb.ToString();
}

static string JsonBuildItemsObject(int count)
{
    var sb = new StringBuilder();
    sb.Append("{\"items\":[");
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            sb.Append(',');
        }
        sb.Append(i);
    }
    sb.Append("]}");
    return sb.ToString();
}

static long RunJsonParse(ulong repetitions)
{
    var payload = JsonBenchmarkPayload();
    long total = 0;
    for (ulong i = 0; i < repetitions; ++i)
    {
        using var doc = JsonDocument.Parse(payload);
        total += doc.RootElement.GetProperty("items").GetArrayLength();
    }
    return total;
}

static long RunJsonStringify(ulong repetitions)
{
    long total = 0;
    for (ulong i = 0; i < repetitions; ++i)
    {
        var encoded = JsonBuildItemsObject(200);
        total += encoded.Length;
    }
    return total;
}

static long RunJsonPipeline(ulong repetitions)
{
    var payload = JsonBenchmarkPayload();
    long total = 0;
    for (ulong i = 0; i < repetitions; ++i)
    {
        using var doc = JsonDocument.Parse(payload);
        var items = doc.RootElement.GetProperty("items");
        long sum = 0;
        foreach (var value in items.EnumerateArray())
        {
            sum += value.GetInt64();
        }
        var encoded = JsonBuildItemsObject(items.GetArrayLength());
        total += sum + encoded.Length;
    }
    return total;
}

static long RunMixedRuntime(ulong repetitions)
{
    var payload = JsonBenchmarkPayload();
    long total = 0;
    for (ulong i = 0; i < repetitions; ++i)
    {
        using var doc = JsonDocument.Parse(payload);
        var items = doc.RootElement.GetProperty("items");
        long sum = 0;
        foreach (var value in items.EnumerateArray())
        {
            sum += value.GetInt64();
        }
        var encoded = JsonBuildItemsObject(items.GetArrayLength());
        using var doc2 = JsonDocument.Parse(encoded);
        total += sum + encoded.Length + doc2.RootElement.GetProperty("items").GetArrayLength();
    }
    return total;
}

static double MedianMs(Action body, int repeats, int warmup = 3)
{
    for (int w = 0; w < warmup; ++w)
    {
        body();
    }
    var samples = new double[repeats];
    for (int i = 0; i < repeats; ++i)
    {
        var sw = Stopwatch.StartNew();
        body();
        sw.Stop();
        samples[i] = sw.Elapsed.TotalMilliseconds;
    }
    Array.Sort(samples);
    return samples[repeats / 2];
}

string workload = "int_loop";
bool workloadExplicit = false;
for (int i = 0; i + 1 < args.Length; ++i)
{
    if (args[i] == "--workload")
    {
        workload = args[i + 1];
        workloadExplicit = true;
    }
}
if (!workloadExplicit)
{
    for (int i = 0; i < args.Length; ++i)
    {
        if (args[i].Length > 0 && args[i][0] != '-')
        {
            workload = args[i];
            break;
        }
        if (args[i] == "--iters")
        {
            ++i;
        }
    }
}
ulong DefaultIters(string name) => name switch
{
    "direct_inc_call" or "add_constant" or "indirect_call" => 400_000,
    "nested_call" => 50_000,
    "closure_call" => 200_000,
    "int_loop" => 2_000_000,
    "list_index" => 5_000,
    "string_eq" => 8_000,
    "string_concat" => 250,
    "arithmetic" => 8_000,
    "dict_lookup" => 400,
    "json_parse" => 200,
    "json_stringify" => 400,
    "json_pipeline" => 100,
    "mixed_runtime" => 50,
    _ => 2_000_000,
};
ulong iters = DefaultIters(workload);
for (int i = 0; i + 1 < args.Length; ++i)
{
    if (args[i] == "--iters")
    {
        iters = ulong.Parse(args[i + 1]);
    }
}

long checksum = 0;
double median;
if (workload == "loops")
{
    median = MedianMs(() => checksum = RunPscriptLoops(), 7);
}
else if (workload == "num_lu_divmod")
{
    median = MedianMs(() => checksum = RunPscriptNumLuDivmod(), 7);
}
else if (workload == "closure_call")
{
    median = MedianMs(() => checksum = RunPscriptClosureCall(), 7);
}
else if (workload == "closure_nested")
{
    median = MedianMs(() => checksum = RunPscriptClosureNested(), 7);
}
else if (workload == "closure_recursive")
{
    median = MedianMs(() => checksum = RunPscriptClosureRecursive(), 7);
}
else if (workload == "closure_recursive_iter")
{
    median = MedianMs(() => checksum = RunPscriptClosureRecursiveIter(), 7);
}
else if (workload == "int_loop")
{
    median = MedianMs(() => checksum = RunIntLoop(iters), 7);
}
else if (workload == "arithmetic")
{
    median = MedianMs(() => checksum = RunArithmetic(iters), 7);
}
else if (workload == "list_index")
{
    median = MedianMs(() => checksum = RunListIndex(iters), 7);
}
else if (workload == "string_eq")
{
    median = MedianMs(() => checksum = RunStringEq(iters), 7);
}
else if (workload == "string_concat")
{
    median = MedianMs(() => checksum = RunStringConcat(iters), 7);
}
else if (workload == "direct_inc_call")
{
    median = MedianMs(() => checksum = RunDirectIncCall(iters), 7);
}
else if (workload == "nested_call")
{
    median = MedianMs(() => checksum = RunNestedCall(iters), 7);
}
else if (workload == "dict_lookup")
{
    median = MedianMs(() => checksum = RunDictLookup(iters), 7);
}
else if (workload == "json_parse")
{
    median = MedianMs(() => checksum = RunJsonParse(iters), 7);
}
else if (workload == "json_stringify")
{
    median = MedianMs(() => checksum = RunJsonStringify(iters), 7);
}
else if (workload == "json_pipeline")
{
    median = MedianMs(() => checksum = RunJsonPipeline(iters), 7);
}
else if (workload == "mixed_runtime")
{
    median = MedianMs(() => checksum = RunMixedRuntime(iters), 7);
}
else if (workload == "closure_call_legacy")
{
    median = MedianMs(() =>
    {
        long capture = 1;
        Func<long, long> callee = x => x + capture;
        long total = 0;
        for (ulong i = 0; i < iters; ++i)
        {
            total += callee((long)i);
        }
        checksum = total;
    }, 7);
}
else if (workload == "add_constant")
{
    median = MedianMs(() =>
    {
        const long constant = 7;
        Func<long, long> add = x => x + constant;
        long total = 0;
        for (ulong i = 0; i < iters; ++i)
        {
            total += add((long)i);
        }
        checksum = total;
    }, 7);
}
else if (workload == "indirect_call")
{
    median = MedianMs(() =>
    {
        Func<long, long> inc = x => x + 1;
        Func<long, long> callee = inc;
        long total = 0;
        for (ulong i = 0; i < iters; ++i)
        {
            total += callee((long)i);
        }
        checksum = total;
    }, 7);
}
else
{
    Console.Error.WriteLine($"unsupported workload: {workload}");
    return 2;
}
Console.WriteLine($"csharp\t{workload}\t{median}\t{checksum}");
return 0;
