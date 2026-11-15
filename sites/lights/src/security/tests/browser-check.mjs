import {fromHex, hex} from '../bytes.mjs';
import {Security2Session} from '../security2.mjs';
import {parseStrictJSON} from '../strict-json.mjs';

const output = document.querySelector('#result');
const lines = []; let passed = 0;
function check(value, label) { if (!value) throw Error(label); passed++; }
try {
  check(globalThis.isSecureContext, 'Secure context required');
  const response = await fetch(new URL('./interop-fixtures.json', import.meta.url), {cache: 'no-store', credentials: 'omit'});
  check(response.ok, 'Fixture fetch');
  const corpus = await response.json();
  check(corpus.idf_revision === 'fff9895c82d744c7237be8847347bdd1b07c6643', 'IDF pin');
  for (const v of corpus.vectors) {
    const values = [v.ephemeral, v.challenge];
    const provider = {subtle: crypto.subtle, getRandomValues(out) {
      check(out.length === 32 && values.length > 0, 'Fixture randomness request');
      out.set(fromHex(values.shift())); return out;
    }};
    const session = new Security2Session({role: 'guest', credential: v.password, crypto: provider});
    check(hex(await session.startHandshake()) === v.request0, `${v.name}: A/protobuf`);
    check(hex(await session.receiveChallenge(fromHex(v.response0))) === v.request1, `${v.name}: client proof`);
    await session.verifyServer(fromHex(v.response1));
    check(session.transcript === v.transcript, `${v.name}: transcript`);
    await session.makeBinding({gateway_id: v.gateway, public_key_spki: v.spki}, v.boot, 'guest');
    await session.verifyBinding(fromHex(v.binding_ciphertext));
    check(session.role === 'guest', `${v.name}: P-256 DER identity proof`);
    check(hex(await session.sealRequest(fromHex(v.plaintext_request))) === v.request_ciphertext, `${v.name}: AES request counter3`);
    check(hex(await session.openResponse(fromHex(v.response_ciphertext))) === v.plaintext_response, `${v.name}: AES response counter4`);
    await session.sealRequest(fromHex(v.plaintext_request));
    let rejected = false;
    try { await session.openResponse(fromHex(v.response_ciphertext)); } catch { rejected = true; }
    check(rejected && session.state === 'closed', `${v.name}: replay fails closed`);
    lines.push(`PASS ${v.name}: SRP, protobuf, transcript, P-256, AES-GCM, replay`);
  }
  for (const bad of ['{"a":1,"\\u0061":2}', '{"a":1}{}', '{"a":9007199254740992}']) {
    let rejected = false; try { parseStrictJSON(bad); } catch { rejected = true; }
    check(rejected, 'Strict JSON negative');
  }
  lines.push(`PASS ${passed} browser assertions`, navigator.userAgent);
  document.documentElement.dataset.result = 'passed';
} catch (error) {
  lines.push(`FAIL after ${passed} assertions: ${error?.code ?? error?.message ?? 'unknown'}`);
  document.documentElement.dataset.result = 'failed';
}
output.textContent = lines.join('\n');
