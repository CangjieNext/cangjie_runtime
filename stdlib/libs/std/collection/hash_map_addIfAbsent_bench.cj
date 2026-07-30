// Benchmark test for HashMap.addIfAbsent performance
// Issue: https://atomgit.com/Cangjie/cangjie_runtime/issues/583
// 社区学习赛

package std_test

import std.collection.*
import std.random.*
import std.time.*

@Test
@Configure[warmup: Duration.second * 10, minDuration: Duration.minute * 1]
class CjPerfBench {
    private static const DATA_SIZE = 256 * 1024
    private static let RND = Random(42)

    private static const STR_MIN_SIZE = 2
    private static const STR_MAX_SIZE = 20

    private static let RANDOM_STRING_ARR = Array<String>(DATA_SIZE) {_ =>
        let size = RND.nextInt64(STR_MAX_SIZE - STR_MIN_SIZE)
        var text = Array<Rune>(STR_MIN_SIZE + size, repeat: Rune(0))
        for (i in 0..(STR_MIN_SIZE + size)) {
            text[i] = RND.suggestRune()
        }
        String(text)
    }

    @Bench
    func checkHashMapAddIfAbsent(): Int64 {
        var count = 0
        let data = HashMap<String, Int64>()
        for (str in RANDOM_STRING_ARR) {
            data.addIfAbsent(str, count)
            count++
        }
        return count
    }

    @Bench
    func checkHashMapContainsAdd(): Int64 {
        var count = 0
        let data = HashMap<String, Int64>()
        for (str in RANDOM_STRING_ARR) {
            if (!data.contains(str)) {
                data.add(str, count)
                count++
            }
        }
        return count
    }
}
