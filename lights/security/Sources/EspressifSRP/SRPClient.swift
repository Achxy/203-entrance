// Copyright (C) 2016 Bouke Haarsma
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Adapted from Espressif provisioning-ios 3.0.3 (ff70c09).
// Only SRP-6a / SHA-512 / 3072-bit group is exposed. See UPSTREAM.md.

import Foundation
import CryptoKit
import Security
import Darwin

public enum SRPError: Error {
    case invalidCredentials, randomFailure, invalidPublicKey, invalidChallenge, invalidProof
}

/// The same pinned upstream x/v formula used by the SRP client. This material
/// provisions a verifier; it is not a client session key or an authentication.
public enum SRPVerifierMaterial {
    public static func make(username: Data, password: Data, salt: Data) throws -> Data {
        guard (1...64).contains(username.count), (1...256).contains(password.count), salt.count == 16 else {
            throw SRPError.invalidCredentials
        }
        var combined = username + Data([0x3a]) + password
        defer { erase(&combined) }
        let x = BigUInt(hash(salt + hash(combined)))
        let group = Group.N3072
        return pad(group.g.power(x, modulus: group.N).serialize(), to: 384)
    }
}

/// Package implementation detail. The public product exposes Security2Session only.
/// Upstream BigUInt modular arithmetic is variable-time, not side-channel hardened.
public final class SRPClient {
    private var a: BigUInt?
    private var password: Data
    private var username: Data
    private let A: Data
    private var expectedProof: Data?
    private var key: SymmetricKey?
    private var challengeUsed = false
    private var verified = false
    private let group = Group.N3072

    public convenience init(username: Data, password: Data) throws {
        var ephemeral = Data(count: 32)
        let result = ephemeral.withUnsafeMutableBytes {
            SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!)
        }
        guard result == errSecSuccess else { throw SRPError.randomFailure }
        defer { erase(&ephemeral) }
        try self.init(username: username, password: password, ephemeral: ephemeral)
    }

    // Deterministic ephemeral injection is package-only, solely for interoperability fixtures.
    package init(username: Data, password: Data, ephemeral: Data) throws {
        guard (1...64).contains(username.count), (1...256).contains(password.count),
              ephemeral.count == 32, BigUInt(ephemeral) != 0 else {
            throw SRPError.invalidCredentials
        }
        self.username = username
        self.password = password
        let secret = BigUInt(ephemeral)
        self.a = secret
        // IDF's handler requires exactly 384 bytes even when A has a leading zero.
        A = pad(group.g.power(secret, modulus: group.N).serialize(), to: 384)
    }

    deinit { clear() }

    public var publicKey: Data { A }

    /// Derives only a proof; the key cannot be accessed until server proof succeeds.
    public func processChallenge(salt: Data, publicKey BData: Data) throws -> Data {
        guard !challengeUsed, let a, (16...64).contains(salt.count),
              (1...384).contains(BData.count) else { throw SRPError.invalidChallenge }
        challengeUsed = true
        defer { erase(&password); self.a = nil }
        let N = group.N
        let B = BigUInt(BData)
        guard B > 0, B < N else { throw SRPError.invalidPublicKey }
        let u = BigUInt(hash(pad(A, to: 384) + pad(BData, to: 384)))
        guard u != 0 else { throw SRPError.invalidChallenge }
        let k = BigUInt(hash(N.serialize() + pad(group.g.serialize(), to: 384)))
        var userPassword = username + Data([0x3a]) + password
        defer { erase(&userPassword) }
        let x = BigUInt(hash(salt + hash(userPassword)))
        let v = group.g.power(x, modulus: N)
        // Espressif's upstream SRP-6a calculation, preserving minimal S serialization.
        let S = (B + N - k * v % N).power(a + u * x, modulus: N)
        var shared = hash(S.serialize())
        defer { erase(&shared) }
        key = SymmetricKey(data: shared.prefix(32))
        let hN = hash(N.serialize())
        let hg = hash(pad(group.g.serialize(), to: 384))
        let xor = Data(zip(hN, hg).map { $0 ^ $1 })
        let proof = hash(xor + hash(username) + salt + A + BData + shared)
        expectedProof = hash(A + proof + shared)
        erase(&username)
        return proof
    }

    public func verifyServer(proof: Data) throws -> SymmetricKey {
        guard !verified, proof.count == 64, let expectedProof, let key else {
            clear()
            throw SRPError.invalidProof
        }
        let equal = proof.withUnsafeBytes { received in
            expectedProof.withUnsafeBytes { expected in
                timingsafe_bcmp(received.baseAddress!, expected.baseAddress!, 64) == 0
            }
        }
        guard equal else { clear(); throw SRPError.invalidProof }
        verified = true
        clear()
        return key
    }

    public func clear() {
        erase(&password)
        erase(&username)
        if expectedProof != nil { erase(&expectedProof!) }
        expectedProof = nil
        key = nil
        a = nil
    }
}

private func hash(_ bytes: Data) -> Data { Data(SHA512.hash(data: bytes)) }

private func pad(_ bytes: Data, to size: Int) -> Data {
    precondition(bytes.count <= size)
    return Data(repeating: 0, count: size - bytes.count) + bytes
}

/// Best-effort buffer clearing; Swift/CryptoKit/BigUInt can have compiler or COW copies.
/// This deliberately makes no guarantee that every historic copy is zeroized.
private func erase(_ data: inout Data) {
    data.withUnsafeMutableBytes { raw in
        if let address = raw.baseAddress { _ = memset_s(address, raw.count, 0, raw.count) }
    }
    data.removeAll(keepingCapacity: false)
}
