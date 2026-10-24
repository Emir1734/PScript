import java.util.Arrays;

public final class CrossLangBench {
    private static final int WARMUP = 3;
    private static final int REPEATS = 7;

    private CrossLangBench() {}

    static long runPscriptLoops() {
        final long n = 60_000L;
        long s = 0;
        for (long i = 0; i < n; ++i) {
            s += 1;
        }
        return s;
    }

    static long runPscriptNumLuDivmod() {
        final long n = 12_000L;
        long total = 0;
        for (long i = 1; i < n; ++i) {
            total += (i / 3) + (i % 7);
        }
        return total;
    }

    interface LongAdd {
        long add(long x);
    }

    interface TriFn {
        long apply(long z);
    }

    interface MidFn {
        TriFn bind(long y);
    }

    interface OutFn {
        MidFn bind(long x);
    }

    static long runPscriptClosureCall() {
        final long n = 25_000L;
        final long baseVal = 100L;
        final LongAdd add = new LongAdd() {
            @Override
            public long add(long x) {
                return baseVal + x;
            }
        };
        long total = 0;
        for (long i = 0; i < n; ++i) {
            total += add.add(i);
        }
        return total;
    }

    static long runPscriptClosureNested() {
        final long n = 25_000L;
        final OutFn outer = x -> y -> z -> x + y + z;
        final TriFn g = outer.bind(10L).bind(20L);
        long total = 0;
        for (long i = 0; i < n; ++i) {
            total += g.apply(i);
        }
        return total;
    }

    static long sumRecursive(long n) {
        if (n <= 0) {
            return 0L;
        }
        return n + sumRecursive(n - 1);
    }

    static long runPscriptClosureRecursive() {
        return sumRecursive(200L);
    }

    static long runPscriptClosureRecursiveIter() {
        long acc = 0L;
        for (long n = 200L; n > 0L; --n) {
            acc += n;
        }
        return acc;
    }

    static double medianMs(Workload workload) {
        for (int w = 0; w < WARMUP; ++w) {
            workload.run();
        }
        double[] samples = new double[REPEATS];
        for (int i = 0; i < REPEATS; ++i) {
            long start = System.nanoTime();
            workload.run();
            long end = System.nanoTime();
            samples[i] = (end - start) / 1_000_000.0;
        }
        Arrays.sort(samples);
        return samples[REPEATS / 2];
    }

    interface Workload {
        long run();
    }

    static void runNamed(String name) {
        final long[] checksum = new long[1];
        Workload body;
        switch (name) {
            case "loops":
                body = () -> checksum[0] = runPscriptLoops();
                break;
            case "num_lu_divmod":
                body = () -> checksum[0] = runPscriptNumLuDivmod();
                break;
            case "closure_call":
                body = () -> checksum[0] = runPscriptClosureCall();
                break;
            case "closure_nested":
                body = () -> checksum[0] = runPscriptClosureNested();
                break;
            case "closure_recursive":
                body = () -> checksum[0] = runPscriptClosureRecursive();
                break;
            case "closure_recursive_iter":
                body = () -> checksum[0] = runPscriptClosureRecursiveIter();
                break;
            case "verify":
                System.out.println("loops\t" + runPscriptLoops());
                System.out.println("num_lu_divmod\t" + runPscriptNumLuDivmod());
                System.out.println("closure_call\t" + runPscriptClosureCall());
                System.out.println("closure_nested\t" + runPscriptClosureNested());
                System.out.println("closure_recursive\t" + runPscriptClosureRecursive());
                System.out.println("closure_recursive_iter\t" + runPscriptClosureRecursiveIter());
                return;
            default:
                System.err.println("unsupported workload: " + name);
                System.exit(2);
                return;
        }
        double median = medianMs(body);
        System.out.printf("java\t%s\t%.6f\t%d%n", name, median, checksum[0]);
    }

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("usage: CrossLangBench <workload>|verify");
            System.exit(2);
        }
        runNamed(args[0]);
    }
}
