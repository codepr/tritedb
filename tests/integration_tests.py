import os
import time
import signal
import struct
import unittest
import subprocess
from socket import socket, htons, htonl, ntohl, ntohs


class TrieDBTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.proc = subprocess.Popen(
            ['bin/triedb'],
            stdout=subprocess.PIPE,
            preexec_fn=os.setsid
        )

        time.sleep(.5)

        cls.connection = socket()
        cls.connection.connect(('127.0.0.1', 9090))

        cls.opcodes = {
            'PUT': 0b00010000,
            'GET': 0b00100000,
            'DEL': 0b00110000,
            'TTL': 0b01000000,
            'INC': 0b01010000,
            'DEC': 0b01100000,
            'CNT': 0b01110000,
            'USE': 0b10000000,
            'KEYS': 0b10010000,
            'PING': 0b10100000,
            'QUIT': 0b10110000,
            'DB': 0b11000000
        }

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()
        os.kill(cls.proc.pid, signal.SIGTERM)

    def _send_put(self, key, value, ttl=-1):
        keylen = len(key)
        vallen = len(value)

        put_opcode = self.opcodes['PUT']
        put = struct.pack(
            f'!BBiH{keylen}s{vallen}s',
            put_opcode,
            keylen + vallen + 6,  # 6 = sizeof(i) + sizeof(H)
            ttl,
            keylen,
            key.encode(),
            value.encode()
        )

        self.connection.send(put)
        header = self.connection.recv(3)
        byte, _, rc = struct.unpack('!BBB', header)
        return byte, rc

    def _send_get(self, key):
        keylen = len(key)
        get = struct.pack(
            f'!BB{keylen}s',
            self.opcodes['GET'],
            keylen,
            key.encode()
        )
        self.connection.send(get)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
            data = code
        else:
            datalen = ntohl(struct.unpack('=I', self.connection.recv(4))[0])
            data = struct.unpack(f'={datalen}s', self.connection.recv(datalen))[0]

        return data

    def _send_del(self, keys):
        is_prefix = False
        totlen = sum(len(k) for k in keys)
        fmtinit = '=BIBI'
        fmt = ''.join(f'H{len(key)}sB' for key in keys)
        totlen += 10 + 3 * len(keys)
        keys_to_net = [x for t in [(htons(len(key)), key.encode(), is_prefix) for key in keys] for x in t]
        fmt = fmtinit + fmt
        delete = struct.pack(
            fmt,
            0x03,
            htonl(totlen),
            0x00,
            htonl(len(keys)),
            *keys_to_net
        )
        self.connection.send(delete)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        payload = struct.unpack('=B', self.connection.recv(total_len - 6))

        return code

    def test_put(self):
        key = "test-key-1"
        value = "test-value-1"

        code = self._send_put(key, value)

        self.assertEqual(code, 0x00)

        self._send_del([key])

    def test_get(self):
        self._send_put("test-key-1", "test-value-1")
        key = "test-key-1"
        keylen = len(key)
        get = struct.pack(
        f'=BIBH{keylen}s',
            0x02,
            htonl(8 + keylen),
            0x00,
            htons(keylen),
            key.encode()
        )
        self.connection.send(get)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
            data = code
        else:
            datalen = ntohl(struct.unpack('=I', self.connection.recv(4))[0])
            data = struct.unpack(f'={datalen}s', self.connection.recv(datalen))[0]

        self.assertEqual(data, b"test-value-1")

        self._send_del([key])

    def test_count(self):
        key = "key"
        fmt = f'=BIBH{len(key)}s'
        count = struct.pack(
            fmt,
            0x07,
            htonl(8 + len(key)),
            0x00,
            htons(len(key)),
            key.encode()
        )
        self.connection.send(count)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        payload = ntohl(struct.unpack('=I', self.connection.recv(total_len - 6))[0])

        self.assertEqual(code, 0x00)
        self.assertEqual(payload, 0)

    def test_del(self):
        self._send_put('key-1', 'value-1')
        keys = ['key']
        is_prefix = True
        totlen = sum(len(k) for k in keys)
        fmtinit = '=BIBI'
        if is_prefix:
            fmt = ''.join(f'H{len(key)}sB' for key in keys)
            totlen += 10 + 3 * len(keys)
            keys_to_net = [x for t in [(htons(len(key)), key.encode(), is_prefix) for key in keys] for x in t]
        else:
            fmt = ''.join(f'H{len(key)}s' for key in keys)
            totlen += 10 + 2 * len(keys)
            keys_to_net = [x for t in [(htons(len(key)), key.encode()) for key in keys] for x in t]
        fmt = fmtinit + fmt
        delete = struct.pack(
            fmt,
            0x03,
            htonl(totlen),
            0x00,
            htonl(len(keys)),
            *keys_to_net
        )
        self.connection.send(delete)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        payload = struct.unpack('=B', self.connection.recv(total_len - 6))

        self.assertEqual(code, 0x00)

    def test_keys(self):
        self._send_put('key1', 'value1')
        self._send_put('key2', 'value2')
        self._send_put('key3', 'value3')
        key = 'key'
        fmt = f'=BIBH{len(key)}s'
        keys = struct.pack(
            fmt,
            0x08,
            htonl(8 + len(key)),
            0x00,
            htons(len(key)),
            key.encode()
        )
        self.connection.send(keys)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        key_len = ntohs(struct.unpack('=H', self.connection.recv(2))[0])
        keys = []

        for _ in range(key_len):
            keylen = ntohs(struct.unpack('=H', self.connection.recv(2))[0])
            keys.append(struct.unpack(f'={keylen}sB', self.connection.recv(keylen+1))[0])

        self.assertEqual(code, 0x00)
        self.assertEqual(len(keys), 3)
        self.assertEqual(keys, [b'key1', b'key2', b'key3'])

        self._send_del(['key1', 'key2', 'key3'])

    def test_ttl(self):
        self._send_put('ttlkey', 'value')
        key = 'ttlkey'
        ttl = 2
        keylen = len(key)
        ttl = struct.pack(
            f'=BIBH{keylen}sBH',
            0x04,
            htonl(11 + keylen),
            0x00,
            htons(keylen),
            key.encode(),
            0,
            htons(ttl)
        )
        self.connection.send(ttl)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
        else:
            klen, vlen = struct.unpack('=HI', self.connection.recv(6))
            klen, vlen = ntohs(klen), ntohl(vlen)
            payload = struct.unpack(f'={klen}s{vlen}s', self.connection.recv(klen + vlen))

        self.assertEqual(code, 0x00)

        data = self._send_get(key)

        self.assertEqual(data, b'value')

        time.sleep(2)

        data = self._send_get(key)

        self.assertEqual(data, 0)

        self._send_del([key])

    def test_inc(self):
        self._send_put('inc-key', '9')
        keys = ['inc-key']
        totlen = sum(len(k) for k in keys)
        fmtinit = '=BIBI'
        is_prefix = False
        fmt = ''.join(f'H{len(key)}sB' for key in keys)
        totlen += 10 + 3 * len(keys)
        keys_to_net = [x for t in [(htons(len(key)), key.encode(), is_prefix) for key in keys] for x in t]
        fmt = fmtinit + fmt
        inc = struct.pack(
            fmt,
            0x05,
            htonl(totlen),
            0x00,
            htonl(len(keys)),
            *keys_to_net
        )
        self.connection.send(inc)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        payload = struct.unpack('=B', self.connection.recv(total_len - 6))

        self.assertEqual(code, 0x00)

        data = self._send_get('inc-key')

        self.assertEqual(data, b'10')

        self._send_del(keys)

    def test_put_bulk(self):

        kvs = {
            'key1': 'value1',
            'key2': 'value2',
            'key3': 'value3',
            'key4': 'value4',
            'key5': 'value5'
        }

        prefix = False

        fmt = ''.join(f'HI{len(key)}s{len(val)}sBH' for key, val in kvs.items())

        keysval = [x for t in [(htons(len(k)), htonl(len(v)), k.encode(), v.encode(), prefix, htons(0)) for k, v in kvs.items()] for x in t]

        totlen = 10 + 9 * len(kvs) + sum(len(k) + len(v) for k, v in kvs.items())

        put = struct.pack(
            f'=BIBI' + fmt,
            0x01,
            htonl(totlen),
            0x00 | 0x01 << 1,
            htonl(len(kvs)),
            *keysval
        )

        self.connection.send(put)
        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        _ = struct.unpack('=B', self.connection.recv(1))

        self.assertEqual(code, 0x00)

        self.assertEqual(self._send_get('key1'), b'value1')

        self._send_del(kvs.keys())

    def test_use(self):

        db = struct.pack('=BIB', 0xfd, htonl(6), 0x00)
        self.connection.send(db)

        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
            payload = code
        else:
            datalen = ntohl(struct.unpack('=I', self.connection.recv(4))[0])
            payload = struct.unpack(f'={datalen}s', self.connection.recv(datalen))[0]

        self.assertEqual(code, 0x02)
        self.assertEqual(payload, b'db0')

        dbname = "test-database"

        use = struct.pack(
            f'=BIBH{len(dbname)}s',
            0x09,
            htonl(8 + len(dbname)),
            0x00,
            htons(len(dbname)),
            dbname.encode()
        )

        self.connection.send(use)

        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        _ = struct.unpack('=B', self.connection.recv(1))

        self.assertEqual(code, 0x00)

        db = struct.pack('=BIB', 0xfd, htonl(6), 0x00)
        self.connection.send(db)

        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
            payload = code
        else:
            datalen = ntohl(struct.unpack('=I', self.connection.recv(4))[0])
            payload = struct.unpack(f'={datalen}s', self.connection.recv(datalen))[0]

        self.assertEqual(code, 0x02)
        self.assertEqual(payload, dbname.encode())

        defaultdb = "db0"

        use = struct.pack(
            f'=BIBH{len(defaultdb)}s',
            0x09,
            htonl(8 + len(defaultdb)),
            0x00,
            htons(len(defaultdb)),
            defaultdb.encode()
        )

        self.connection.send(use)

        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        _ = struct.unpack('=B', self.connection.recv(1))

        self.assertEqual(code, 0x00)

        db = struct.pack('=BIB', 0xfd, htonl(6), 0x00)
        self.connection.send(db)

        header = self.connection.recv(6)
        code, total_len, *_ = struct.unpack('=BIB', header)
        total_len = ntohl(total_len)
        if code == 0x00:
            payload = struct.unpack('=B', self.connection.recv(total_len - 6))
            payload = code
        else:
            datalen = ntohl(struct.unpack('=I', self.connection.recv(4))[0])
            payload = struct.unpack(f'={datalen}s', self.connection.recv(datalen))[0]

        self.assertEqual(code, 0x02)
        self.assertEqual(payload, defaultdb.encode())
