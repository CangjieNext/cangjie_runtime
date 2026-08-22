import std.io.{ByteBuffer, readToEnd}

func ensure(condition: Bool): Unit {
    if (!condition) {
        throw Exception("case25 stream transformation assertion failed")
    }
}

main() {
    ensure(initialResponseCapacity(Some(99_572)) == 107_764)
    ensure(initialResponseCapacity(None) == 8_192)

    let input = ByteBuffer()
    unsafe {
        input.write(##"{"keep":{"nested":[1,true,null]},"counters":{"in":4,"ok":9},"events":[{"seq":8,"score":1.5,"user":{"attrs":{"credits":2.5}},"custom":{"product_ids":[11,12]}}]}"##.rawData())
    }
    let output = ByteBuffer()

    transformRequest(input, output, 7)

    let actual = String.fromUtf8(readToEnd(output))
    let expected = ##"{"server_ok":true,"processed_seq":7,"keep":{"nested":[1,true,null]},"counters":{"in":5,"ok":10},"events":[{"seq":9,"score":3,"user":{"attrs":{"credits":5}},"custom":{"product_ids":[11,12,9999]},"server_marker":"m-0"}]}"##
    ensure(actual == expected)

    let validResponse = ByteBuffer()
    unsafe { validResponse.write(actual.rawData()) }
    ensure(responseHasServerOk(validResponse))

    let invalidResponse = ByteBuffer()
    unsafe { invalidResponse.write(##"{"server_ok":false}"##.rawData()) }
    ensure(!responseHasServerOk(invalidResponse))

    let lateServerOk = ByteBuffer()
    unsafe { lateServerOk.write(##"{"ignored":0,"server_ok":true}"##.rawData()) }
    ensure(!responseHasServerOk(lateServerOk))
}
