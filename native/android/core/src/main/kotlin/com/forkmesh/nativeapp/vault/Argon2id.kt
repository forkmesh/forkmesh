package com.forkmesh.nativeapp.vault

object Argon2id {
    fun deriveKey(
        password: ByteArray,
        salt: ByteArray,
        parallelism: Int,
        memoryKiB: Int,
        iterations: Int,
        hashLength: Int,
        secret: ByteArray = ByteArray(0),
        associatedData: ByteArray = ByteArray(0),
    ): ByteArray {
        require(parallelism in 1 until (1 shl 24)) { "Argon2 parallelism out of range" }
        require(memoryKiB >= 8 * parallelism) { "Argon2 memory out of range" }
        require(iterations >= 1) { "Argon2 iterations out of range" }
        require(hashLength >= 4) { "Argon2 hash length out of range" }
        require(salt.size >= 8) { "Argon2 salt must be at least 8 bytes" }

        val lanes = parallelism
        val segmentLength = memoryKiB / (4 * lanes)
        val laneLength = segmentLength * 4
        val blockCount = laneLength * lanes

        val h0 = Blake2b(64)
        for (value in intArrayOf(lanes, hashLength, memoryKiB, iterations, 0x13, 2)) h0.update(le32(value))
        for (field in arrayOf(password, salt, secret, associatedData)) {
            h0.update(le32(field.size))
            h0.update(field)
        }
        val seed = h0.digest()

        val memory = LongArray(blockCount * 128)
        val scratch = LongArray(5 * 128)
        val zero = 2 * 128
        val input = 3 * 128
        val address = 4 * 128
        try {
            for (lane in 0 until lanes) {
                for (index in 0 until 2) {
                    val block = longHash(1024, seed + le32(index) + le32(lane))
                    load(block, memory, (lane * laneLength + index) * 128)
                    block.fill(0)
                }
            }
            seed.fill(0)

            for (pass in 0 until iterations) {
                for (slice in 0 until 4) {
                    val independent = pass == 0 && slice < 2
                    for (lane in 0 until lanes) {
                        if (independent) {
                            scratch.fill(0, input, input + 128)
                            scratch[input] = pass.toLong()
                            scratch[input + 1] = lane.toLong()
                            scratch[input + 2] = slice.toLong()
                            scratch[input + 3] = blockCount.toLong()
                            scratch[input + 4] = iterations.toLong()
                            scratch[input + 5] = 2
                        }
                        var start = 0
                        if (pass == 0 && slice == 0) {
                            start = 2
                            if (independent) nextAddresses(scratch, address, input, zero)
                        }
                        var current = lane * laneLength + slice * segmentLength + start
                        var previous = if (current % laneLength == 0) current + laneLength - 1 else current - 1
                        var i = start
                        while (i < segmentLength) {
                            if (current % laneLength == 1) previous = current - 1
                            val random: Long = if (independent) {
                                if (i % 128 == 0) nextAddresses(scratch, address, input, zero)
                                scratch[address + i % 128]
                            } else {
                                memory[previous * 128]
                            }
                            var referenceLane = ((random ushr 32).toULong() % lanes.toULong()).toInt()
                            if (pass == 0 && slice == 0) referenceLane = lane
                            val sameLane = referenceLane == lane
                            val area: Int = if (pass == 0) {
                                if (slice == 0) {
                                    i - 1
                                } else if (sameLane) {
                                    slice * segmentLength + i - 1
                                } else {
                                    slice * segmentLength + (if (i == 0) -1 else 0)
                                }
                            } else if (sameLane) {
                                laneLength - segmentLength + i - 1
                            } else {
                                laneLength - segmentLength + (if (i == 0) -1 else 0)
                            }
                            val j1 = random and 0xffffffffL
                            val x = (j1 * j1) ushr 32
                            val relative = area.toLong() - 1 - ((area.toLong() * x) ushr 32)
                            val startPosition = if (pass != 0 && slice != 3) (slice + 1) * segmentLength else 0
                            val reference = ((startPosition + relative) % laneLength).toInt()
                            fillBlock(
                                memory, previous * 128,
                                memory, (referenceLane * laneLength + reference) * 128,
                                memory, current * 128,
                                withXor = pass != 0,
                                scratch = scratch,
                            )
                            i++
                            current++
                            previous++
                        }
                    }
                }
            }

            val final = LongArray(128)
            for (lane in 0 until lanes) {
                val last = (lane * laneLength + laneLength - 1) * 128
                for (w in 0 until 128) final[w] = final[w] xor memory[last + w]
            }
            val bytes = ByteArray(1024)
            for (w in 0 until 128) for (b in 0 until 8) bytes[8 * w + b] = (final[w] ushr (8 * b)).toByte()
            final.fill(0)
            val tag = longHash(hashLength, bytes)
            bytes.fill(0)
            return tag
        } finally {
            memory.fill(0)
            scratch.fill(0)
        }
    }

    private fun le32(value: Int): ByteArray =
        byteArrayOf(value.toByte(), (value ushr 8).toByte(), (value ushr 16).toByte(), (value ushr 24).toByte())

    private fun load(bytes: ByteArray, into: LongArray, offset: Int) {
        for (w in 0 until 128) {
            var word = 0L
            for (b in 0 until 8) word = word or ((bytes[8 * w + b].toLong() and 0xff) shl (8 * b))
            into[offset + w] = word
        }
    }

    fun longHash(length: Int, input: ByteArray): ByteArray {
        if (length <= 64) return Blake2b.hash(length, le32(length), input)
        val out = ByteArray(length)
        var v = Blake2b.hash(64, le32(length), input)
        System.arraycopy(v, 0, out, 0, 32)
        var offset = 32
        var remaining = length - 32
        while (remaining > 64) {
            v = Blake2b.hash(64, v)
            System.arraycopy(v, 0, out, offset, 32)
            offset += 32
            remaining -= 32
        }
        val tail = Blake2b.hash(remaining, v)
        System.arraycopy(tail, 0, out, offset, remaining)
        v.fill(0)
        return out
    }

    private fun nextAddresses(scratch: LongArray, address: Int, input: Int, zero: Int) {
        scratch[input + 6] = scratch[input + 6] + 1
        fillBlock(scratch, zero, scratch, input, scratch, address, withXor = false, scratch = scratch)
        fillBlock(scratch, zero, scratch, address, scratch, address, withXor = false, scratch = scratch)
    }

    private fun mix(v: LongArray, ai: Int, bi: Int, ci: Int, di: Int) {
        var a = v[ai]
        var b = v[bi]
        var c = v[ci]
        var d = v[di]
        a = a + b + 2 * (a and 0xffffffffL) * (b and 0xffffffffL)
        d = (d xor a).rotateRight(32)
        c = c + d + 2 * (c and 0xffffffffL) * (d and 0xffffffffL)
        b = (b xor c).rotateRight(24)
        a = a + b + 2 * (a and 0xffffffffL) * (b and 0xffffffffL)
        d = (d xor a).rotateRight(16)
        c = c + d + 2 * (c and 0xffffffffL) * (d and 0xffffffffL)
        b = (b xor c).rotateRight(63)
        v[ai] = a
        v[bi] = b
        v[ci] = c
        v[di] = d
    }

    private fun round(v: LongArray, i: IntArray) {
        mix(v, i[0], i[4], i[8], i[12])
        mix(v, i[1], i[5], i[9], i[13])
        mix(v, i[2], i[6], i[10], i[14])
        mix(v, i[3], i[7], i[11], i[15])
        mix(v, i[0], i[5], i[10], i[15])
        mix(v, i[1], i[6], i[11], i[12])
        mix(v, i[2], i[7], i[8], i[13])
        mix(v, i[3], i[4], i[9], i[14])
    }

    private val ROW_ROUNDS = Array(8) { r -> IntArray(16) { 16 * r + it } }
    private val COLUMN_ROUNDS = Array(8) { r ->
        val o = 2 * r
        intArrayOf(o, o + 1, o + 16, o + 17, o + 32, o + 33, o + 48, o + 49, o + 64, o + 65, o + 80, o + 81, o + 96, o + 97, o + 112, o + 113)
    }

    private fun fillBlock(
        prevArr: LongArray, prev: Int,
        refArr: LongArray, ref: Int,
        nextArr: LongArray, next: Int,
        withXor: Boolean,
        scratch: LongArray,
    ) {
        val r = R.get()
        for (w in 0 until 128) {
            val value = refArr[ref + w] xor prevArr[prev + w]
            r[w] = value
            scratch[128 + w] = if (withXor) value xor nextArr[next + w] else value
        }
        for (rows in ROW_ROUNDS) round(r, rows)
        for (columns in COLUMN_ROUNDS) round(r, columns)
        for (w in 0 until 128) nextArr[next + w] = scratch[128 + w] xor r[w]
    }

    private val R = ThreadLocal.withInitial { LongArray(128) }
}
