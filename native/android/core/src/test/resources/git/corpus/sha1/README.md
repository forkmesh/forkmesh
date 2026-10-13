# SHA-1 collision corpus policy

No collision binary is redistributed in this directory.

The authoritative SHAttered pair is published at [shattered.io](https://shattered.io/static/shattered-1.pdf) and [shattered.io](https://shattered.io/static/shattered-2.pdf).

The source publication is the SHAttered research announcement and its linked paper, `The first collision for full SHA-1`.

The two files have the shared SHA-1 digest `38762cf7f55934b34d179ae6a4c80cadccbb7f0a` according to the publisher.

ForkMesh has not recorded an authoritative SHA-256 checksum or an unambiguous redistribution license for those binary files in this workspace.

For that reason, this directory intentionally contains no binary corpus and normal tests never fetch one.

A human cryptography reviewer who has independently confirmed the source terms may obtain both files directly from the publisher, calculate SHA-256 locally, record the source URL, publication, license or redistribution permission, SHA-256 digest, shared SHA-1 relationship, and expected rejection result before approving any checked-in fixture.

Both members must be rejected as framed Git-object input by any future generalized detector, regardless of chunk boundaries or whether the bytes are presented through loose-object or reconstructed-pack paths.

This repository does not treat a known-sample blacklist as a generalized collision detector.
