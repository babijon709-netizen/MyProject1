import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import tls from 'node:tls';

const run = promisify(execFile);
const [binary, keyPath, certPath] = process.argv.slice(2);
const nonce = 'a'.repeat(32);
const server = tls.createServer({key: readFileSync(keyPath), cert: readFileSync(certPath)}, socket => {
  let input = '';
  socket.on('data', chunk => {
    input += chunk.toString();
    const end = input.indexOf('\r\n\r\n');
    if (end < 0) return;
    const size = Number(/content-length:\s*(\d+)/i.exec(input.slice(0, end))?.[1] ?? -1);
    if (size < 0 || input.length - end - 4 < size) return;
    const data = JSON.parse(input.slice(end + 4, end + 4 + size));
    assert.equal(data.nonce, nonce);
    assert.equal(data.hwid, 'b'.repeat(64));
    assert.match(input, /^POST \/auth HTTP\/1\.1/);
    let content = `enabled:${nonce}\n`;
    let status = 200;
    if (data.mode === 'disabled') content = 'disabled\n';
    if (data.mode === 'replay') content = `enabled:${'0'.repeat(32)}\n`;
    if (data.mode === 'error') {status = 503; content = 'disabled\n';}
    if (data.mode === 'redirect') {status = 302; content = 'enabled\n';}
    if (data.mode === 'chunked') {
      socket.end(`HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n${content.length.toString(16)}\r\n${content}\r\n0\r\n\r\n`);
    } else {
      socket.end(`HTTP/1.1 ${status} Test\r\nContent-Length: ${content.length}\r\nConnection: close\r\n\r\n${content}`);
    }
  });
});
try {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = String(server.address().port);
  for (const [mode, expected] of [
    ['enabled', 0], ['disabled', 1], ['replay', 1], ['error', 2],
    ['redirect', 2], ['chunked', 0],
  ]) {
    const {stdout} = await run(binary, [port, 'localhost', mode], {timeout: 5000});
    assert.equal(Number(stdout.trim()), expected, `${mode}: ${stdout}`);
  }
  const {stdout} = await run(binary, [port, '127.0.0.1', 'enabled'], {timeout: 5000});
  assert.equal(Number(stdout.trim()), 2, 'A CA-signed certificate with the wrong hostname must fail');
  console.log('Bundled HTTPS certificate/nonce/status/chunked tests: OK');
} finally {
  await new Promise(resolve => server.close(resolve));
}
