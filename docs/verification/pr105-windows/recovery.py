"""Disposable native acceptance test. Synthetic WAV/mock HTTP, never microphone.
Run only with an unlocked desktop and no LemonVox instance. Owns a native EDIT,
temporary registry profile, popup menu, clipboard, and loopback HTTP server.
"""
import ctypes as C
from ctypes import wintypes as W
import http.server, json, threading, time, wave, pathlib, subprocess, winreg, sys
U=C.WinDLL('user32',use_last_error=True); K=C.WinDLL('kernel32',use_last_error=True)
U.SetProcessDpiAwarenessContext.argtypes=[C.c_void_p]
U.SetProcessDpiAwarenessContext(C.c_void_p(-4))
for name in ['CreateWindowExW','FindWindowW','GetForegroundWindow','GetFocus','GetClipboardData','GetMenuItemRect','SendMessageW','GetWindowTextLengthW']:
    getattr(U,name).restype=C.c_ssize_t
U.CreateWindowExW.argtypes=[W.DWORD,W.LPCWSTR,W.LPCWSTR,W.DWORD,C.c_int,C.c_int,C.c_int,C.c_int,W.HWND,W.HMENU,W.HINSTANCE,C.c_void_p]
U.SendMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
U.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
U.FindWindowW.argtypes=[W.LPCWSTR,W.LPCWSTR]
U.GetMenuState.argtypes=[W.HMENU,W.UINT,W.UINT]
U.GetMenuState.restype=W.UINT
for name in ['SetForegroundWindow','SetFocus','DestroyWindow']:
    getattr(U,name).argtypes=[W.HWND]
U.ShowWindow.argtypes=[W.HWND,C.c_int]
U.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
U.GetMenuItemRect.argtypes=[W.HWND,W.HMENU,W.UINT,C.POINTER(W.RECT)]
K.GlobalLock.restype=C.c_void_p; K.GlobalLock.argtypes=[W.HGLOBAL]
K.GlobalUnlock.argtypes=[W.HGLOBAL]
state={'mode':'ok','raw':'first raw','clean':'First cleaned.','asr':0,'cleanup':0}
class HTTP(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def reply(self,value,status=200):
        data=json.dumps(value).encode(); self.send_response(status); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        self.reply({'object':'list','data':[{'id':'asr','object':'model','owned_by':'lemonade','recipe':'whispercpp','downloaded':True,'labels':['transcription']},{'id':'llm','object':'model','owned_by':'lemonade','recipe':'llamacpp','downloaded':True,'labels':['chat']}]})
    def do_POST(self):
        mode,raw,clean=state['mode'],state['raw'],state['clean']
        self.rfile.read(int(self.headers.get('Content-Length',0)))
        if self.path.endswith('/audio/transcriptions'):
            state['asr']+=1
            if mode=='cancel': time.sleep(3)
            self.reply({'text':raw},500 if mode=='asr_fail' else 200)
        else:
            state['cleanup']+=1
            self.reply({'choices':[{'message':{'role':'assistant','content':clean},'finish_reason':'stop'}]},500 if mode=='cleanup_fail' else 200)
def pump(seconds=.1):
    end=time.time()+seconds; msg=W.MSG()
    while time.time()<end:
        while U.PeekMessageW(C.byref(msg),None,0,0,1): U.TranslateMessage(C.byref(msg)); U.DispatchMessageW(C.byref(msg))
        time.sleep(.01)
def until(fn,timeout=12):
    end=time.time()+timeout
    while time.time()<end:
        if fn(): return
        pump()
    raise AssertionError('timed out: '+str(fn))
def key(v):
    U.keybd_event(v,0,0,0); U.keybd_event(v,0,2,0)
def clip():
    for _ in range(30):
        if U.OpenClipboard(None): break
        pump(.02)
    else: raise AssertionError('clipboard busy')
    try:
        h=U.GetClipboardData(13)
        if not h:return ''
        p=K.GlobalLock(h)
        try:return C.wstring_at(p)
        finally:K.GlobalUnlock(h)
    finally:U.CloseClipboard()
def text():
    b=C.create_unicode_buffer(8192); U.GetWindowTextW(edit,b,8192); return b.value
def focus():
    U.ShowWindow(window,5); U.SetForegroundWindow(window); U.SetFocus(edit); pump(.25)
    assert U.GetForegroundWindow()==window and U.GetFocus()==edit,'target not foreground; refusing input'
def menu_copy(expect_enabled=True):
    # Exact tray popup codepath. Native popup query is inspection, not automation framework.
    U.AllowSetForegroundWindow(0xffffffff)
    U.PostMessageW(tray,0x8001,(600 | (650<<16)),0x7b)
    until(lambda:U.FindWindowW('#32768',None))
    menu_window=U.FindWindowW('#32768',None); menu=U.SendMessageW(menu_window,0x1e1,0,0)
    assert menu,'popup menu handle unavailable'
    flags=U.GetMenuState(menu,2,0x400); assert flags!=0xffffffff,'invalid popup menu'
    print('menu',hex(menu),'flags',flags,'foreground',U.GetForegroundWindow()==tray,flush=True)
    enabled=(flags&3)==0
    assert enabled==expect_enabled,('copy enabled',enabled,expect_enabled)
    if enabled:
        rect=W.RECT(); assert U.GetMenuItemRect(None,menu,2,C.byref(rect))
        U.SetCursorPos((rect.left+rect.right)//2,(rect.top+rect.bottom)//2);pump(.2)
        U.mouse_event(2,0,0,0,0);U.mouse_event(4,0,0,0,0)
    else:key(27)
    pump(.3)
def record(mode,raw,clean):
    state.update(mode=mode,raw=raw,clean=clean); focus()
    # Native keyboard injection, same real double-Alt hook as app_e2e_test.
    key(0xa4); key(0xa4); pump(.5); before=state['asr']; key(0xa4); key(0xa4)
    until(lambda:state['asr']>before)
    if mode=='cancel':
        U.PostMessageW(tray,0x8001,(400 | (350<<16)),0x7b)
        until(lambda:U.FindWindowW('#32768',None)); key(0x24); key(0x28); key(13)
        pump(4)
    else:pump(2)
def check(label,expected):
    sentinel='PR105 distinct clipboard sentinel '+label
    w=U.CreateWindowExW(0,'EDIT',sentinel,0,0,0,1,1,None,None,None,None)
    U.SendMessageW(w,0xb1,0,-1);U.SendMessageW(w,0x301,0,0);U.DestroyWindow(w)
    assert clip()==sentinel,'sentinel setup failed'
    menu_copy()
    assert clip()==expected,(label,'clipboard mismatch'); print('PASS',label,flush=True)
base=pathlib.Path(__file__).parent; wav=base/'synthetic.wav'
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),HTTP); threading.Thread(target=server.serve_forever,daemon=True).start()
with wave.open(str(wav),'wb') as f:f.setnchannels(1);f.setsampwidth(2);f.setframerate(16000);f.writeframes(b'\0\0'*16000)
profile='Software\\LemonVoxTest\\pr105-recovery-'+str(K.GetCurrentProcessId())
app=sys.argv[1] if len(sys.argv)>1 else r'C:\temp\lemonvox-pr105\src\app\LemonVox.exe'
window=U.CreateWindowExW(0,'STATIC','PR105 recovery acceptance - synthetic audio',0x10cf0000,100,100,1000,650,None,None,None,None)
edit=U.CreateWindowExW(0x200,'EDIT','',0x50000004,10,10,960,570,window,None,None,None)
G=C.WinDLL('gdi32');G.CreateFontW.restype=W.HANDLE
font=G.CreateFontW(32,0,0,0,400,0,0,0,1,0,0,0,0,'Segoe UI')
U.SendMessageW(edit,0x30,font,1)
assert window and edit
original=clip(); process=None
def launch():
    global process,tray
    process=subprocess.Popen([app,'--settings-key',profile,'--sample-wav',str(wav)])
    until(lambda:U.FindWindowW('LemonVoxTrayHost',None)); tray=U.FindWindowW('LemonVoxTrayHost',None); pump(1)
def quit_app():
    global process
    if process:
        process.terminate(); process.wait(timeout=10); process=None; pump(.3)
try:
    assert not U.FindWindowW('LemonVoxTrayHost',None),'LemonVox already running'
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER,profile) as k:
        for name,value in {'Endpoint':f'http://127.0.0.1:{server.server_port}/v1','AsrModel':'asr','CleanupModel':'llm','CleanupBackend':'generic'}.items():winreg.SetValueEx(k,name,0,winreg.REG_SZ,value)
    launch(); menu_copy(False); print('PASS initial disabled',flush=True)
    record('ok','first raw','First cleaned.'); assert text()=='First cleaned.',text()
    check('first copy','First cleaned.'); check('repeated copy','First cleaned.')
    record('ok','second raw','Second cleaned.'); check('replacement','Second cleaned.')
    record('cleanup_fail','raw after cleanup failure','unused'); check('cleanup failure raw recovery','raw after cleanup failure')
    record('asr_fail','unused','unused'); check('ASR failure preserves','raw after cleanup failure')
    print('PENDING native cancellation scenario: ambiguous Cancel menu selection; covered by core tests only.',flush=True)
    quit_app(); launch(); menu_copy(False); print('PASS restart disabled',flush=True)
    print('PENDING insertion failure / cancel during insertion; synthetic mock-server only, no microphone or real Lemonade.',flush=True)
    if '--hold' in sys.argv:
        record('ok','final raw','PR105 recovery acceptance: synthetic text copied successfully.')
        check('final screenshot copy','PR105 recovery acceptance: synthetic text copied successfully.')
        U.PostMessageW(tray,0x8001,(600 | (650<<16)),0x7b)
        print('SCREENSHOT_READY final recovery copy',flush=True)
        end=time.time()+180
        while time.time()<end and not (base/'release').exists():pump(.1)
finally:
    quit_app(); server.shutdown(); U.DestroyWindow(window)
    try:winreg.DeleteKey(winreg.HKEY_CURRENT_USER,profile)
    except FileNotFoundError:pass
    # Preserve Unicode text across harness. Rich clipboard formats are not preserved;
    # parent should use a dedicated clipboard sentinel or snapshot before execution.
    if original:
        # Use native EDIT copy to avoid pointer ownership mistakes.
        w=U.CreateWindowExW(0,'EDIT',original,0,0,0,1,1,None,None,None,None)
        U.SendMessageW(w,0xb1,0,-1); U.SendMessageW(w,0x301,0,0); U.DestroyWindow(w)
