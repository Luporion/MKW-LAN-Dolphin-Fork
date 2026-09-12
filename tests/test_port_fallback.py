"""Test the actual C bind hook with GCC; --windows additionally checks real sockets."""
from pathlib import Path
import subprocess
import tempfile
import sys
import socket

root = Path(__file__).resolve().parent
if '--windows' not in sys.argv:
    source = (root.parent/'src/brainslug-wii-vanilla/modules/rmc-local-net/network_thread.c').read_text()
    start = source.index('static so_ret_t lan_game_SOBind(')
    hook = source[start:source.index('\n}', start)+2]
    code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define AF_INET 2
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
typedef int so_ret_t;
typedef int so_fd_t;
typedef struct { uint8_t sa_len, sa_family; uint16_t sa_port; uint32_t sa_addr; } so_addr_t;
static bool initialising_game_socket;
static int network_thread, other_thread;
static int *current_thread;
static int broadcast_socket=7;
static int calls, first_result, fallback_result;
static so_addr_t seen[2];
static int *OSGetCurrentThread(void) { return current_thread; }
static int SOBind(int fd, const so_addr_t *addr) {
    assert(fd>=0 && calls<2);
    if (addr) seen[calls]=*addr;
    return calls++ ? fallback_result : first_result;
}
'''
    code += hook
    code += r'''
int main(void) {
    /* Success, conflict, access denied, fallback failure, outside setup,
       other thread, discovery socket, already automatic, other family, NULL. */
    for(int test=0; test<10; test++) {
        so_addr_t addr={8,AF_INET,58118,0x12345678};
        initialising_game_socket=true; current_thread=&network_thread;
        calls=0; first_result=-6; fallback_result=0;
        if(test==0) first_result=0;
        if(test==2) first_result=-28;
        if(test==3) fallback_result=-49;
        if(test==4) initialising_game_socket=false;
        if(test==5) current_thread=&other_thread;
        if(test==7) addr.sa_port=0;
        if(test==8) addr.sa_family=99;
        int result=lan_game_SOBind(test==6 ? broadcast_socket : 9, test==9 ? NULL : &addr);
        bool retry=test>=1 && test<=3;
        assert(calls==(retry ? 2 : 1));
        assert(result==(retry ? fallback_result : first_result));
        if(retry) {
            assert(seen[1].sa_port==0 && addr.sa_port==58118);
            assert(seen[1].sa_addr==addr.sa_addr);
            assert(seen[1].sa_len==addr.sa_len && seen[1].sa_family==AF_INET);
        }
    }
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp)/'test.c'
        exe = Path(tmp)/'test'
        c.write_text(code)
        subprocess.run(['gcc','-std=gnu99','-Wall','-Werror',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('PASS: actual C hook, ten cases; maximum two binds; address unchanged; scope and error propagation')
else:
    # Real Windows tests: no traffic is sent, every socket is closed here.
    held=[]
    try:
        owner=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        held.append(owner)
        owner.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
        owner.bind(('0.0.0.0',0))
        port=owner.getsockname()[1]
        client=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        held.append(client)
        try:
            client.bind(('0.0.0.0',port))
            raise AssertionError('Expected occupied port to fail')
        except OSError:
            client.bind(('0.0.0.0',0))
        assert client.getsockname()[1] != port
        print('PASS: occupied port -> port 0 on the SAME Windows socket')
        reserved=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        held.append(reserved)
        try:
            reserved.bind(('0.0.0.0',58118))
            print('SKIP: port 58118 is no longer excluded on this machine')
        except OSError as error:
            reserved.bind(('0.0.0.0',0))
            assert reserved.getsockname()[1]>0
            print('PASS: port 58118 rejected, then automatic bind succeeded:',error.winerror)
        for _ in range(12):
            s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
            held.append(s)
            s.bind(('0.0.0.0',0))
        ports=[s.getsockname()[1] for s in held[-12:]]
        assert len(set(ports))==12
        print('PASS: twelve simultaneous sockets receive distinct ports')
    finally:
        for s in held:
            s.close()
