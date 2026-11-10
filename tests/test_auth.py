import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Session(ctypes.Structure):
    _fields_=[('token',ctypes.c_char*65),('csrf',ctypes.c_char*33),('expires',ctypes.c_int64)]
class State(ctypes.Structure):
    _fields_=[('sessions',Session*2),('failures',ctypes.c_uint),('window',ctypes.c_int64)]
class AuthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();out=Path(cls.temp.name)/'auth.so'
        subprocess.run(['cc','-std=c11','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'components/entrance/include'),
                        str(ROOT/'components/entrance/auth.c'),str(ROOT/'components/entrance/core.c'),
                        '-o',str(out)],check=True)
        cls.lib=ctypes.CDLL(str(out))
        cls.lib.auth_throttled.argtypes=[ctypes.POINTER(State),ctypes.c_int64];cls.lib.auth_throttled.restype=ctypes.c_bool
        cls.lib.auth_failed.argtypes=[ctypes.POINTER(State)]
        cls.lib.auth_issue.argtypes=[ctypes.POINTER(State),ctypes.c_int64,ctypes.c_char_p,ctypes.c_char_p]
        cls.lib.auth_validate.argtypes=[ctypes.POINTER(State),ctypes.c_int64,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_bool]
        cls.lib.auth_revoke.argtypes=[ctypes.POINTER(State),ctypes.c_int]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def setUp(self):self.state=State();self.p=ctypes.byref(self.state);self.token=b'a'*64;self.csrf=b'b'*32
    def test_login_throttle_and_expiry(self):
        for i in range(5):
            self.assertFalse(self.lib.auth_throttled(self.p,1));self.lib.auth_failed(self.p)
        self.assertTrue(self.lib.auth_throttled(self.p,59999999))
        self.assertFalse(self.lib.auth_throttled(self.p,60000000))
        self.assertEqual(self.state.failures,0)
    def test_cookie_mutation_requires_matching_csrf(self):
        self.lib.auth_issue(self.p,1,self.token,self.csrf)
        for value in [None,b'',b'x'*32,b'b'*31,b'b'*33]:
            self.assertEqual(self.lib.auth_validate(self.p,2,self.token,value,True),-1)
        self.assertEqual(self.lib.auth_validate(self.p,2,self.token,self.csrf,True),0)
        self.assertEqual(self.lib.auth_validate(self.p,2,self.token,None,False),0)
    def test_each_token_byte_is_checked(self):
        self.lib.auth_issue(self.p,1,self.token,self.csrf)
        for index in range(64):
            bad=bytearray(self.token);bad[index]^=1
            self.assertEqual(self.lib.auth_validate(self.p,2,bytes(bad),None,False),-1)
        for bad in [None,b'',self.token[:-1],self.token+b'a']:
            self.assertEqual(self.lib.auth_validate(self.p,2,bad,None,False),-1)
    def test_session_revocation_expiry_and_replacement(self):
        self.lib.auth_issue(self.p,1,self.token,self.csrf)
        self.assertEqual(self.lib.auth_validate(self.p,3600000001,self.token,None,False),-1)
        self.lib.auth_issue(self.p,2,b'c'*64,self.csrf)
        self.lib.auth_issue(self.p,3,b'd'*64,self.csrf)
        self.assertEqual(self.lib.auth_validate(self.p,4,self.token,None,False),-1)
        self.assertEqual(self.lib.auth_validate(self.p,4,b'c'*64,None,False),1)
        self.lib.auth_revoke(self.p,1)
        self.assertEqual(self.lib.auth_validate(self.p,4,b'c'*64,None,False),-1)
