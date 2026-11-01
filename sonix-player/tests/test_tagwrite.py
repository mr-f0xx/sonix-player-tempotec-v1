#!/usr/bin/env python3
"""Byte-level tag rewrite tests, independent of the player and audio libraries."""
import ctypes
import os
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
def ss(n): return bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
def sync(b): return b[0] << 21 | b[1] << 14 | b[2] << 7 | b[3]
def frame(name, data, version=4, flags=b'\0\0'):
    return name + (ss(len(data)) if version == 4 else struct.pack('>I', len(data))) + flags + data
def id3(frames, version=4, flags=0):
    tag=b''.join(frames)+b'\0'*12
    return b'ID3'+bytes([version,0,flags])+ss(len(tag))+tag+AUDIO
def parse_id3(data):
    end=10+sync(data[6:10]); pos=10; frames=[]
    while pos<end and data[pos]:
        n=sync(data[pos+4:pos+8]) if data[3]==4 else int.from_bytes(data[pos+4:pos+8],'big')
        frames.append((data[pos:pos+4],data[pos+10:pos+10+n])); pos+=10+n
    return frames,data[end:]
def block(t,data,last=False): return bytes([t | (128 if last else 0)])+len(data).to_bytes(3,'big')+data
def vorbis(fields):
    vendor=b'test vendor'
    return struct.pack('<I',len(vendor))+vendor+struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(x))+x for x in fields)
def parse_flac(data):
    assert data[:4]==b'fLaC'; pos=4; blocks=[]
    while True:
        h=data[pos:pos+4]; n=int.from_bytes(h[1:4],'big'); pos+=4
        blocks.append((h[0]&127,data[pos:pos+n])); pos+=n
        if h[0]&128: break
    return blocks,data[pos:]
def fields(data):
    pos=4+int.from_bytes(data[:4],'little'); count=int.from_bytes(data[pos:pos+4],'little'); pos+=4; result=[]
    for _ in range(count):
        n=int.from_bytes(data[pos:pos+4],'little'); pos+=4; result.append(data[pos:pos+n]); pos+=n
    assert pos==len(data)
    return result
AUDIO=b'\xff\xfb'+b'audio content'*100+b'TAG'+b'x'*125
with tempfile.TemporaryDirectory() as td:
    tmp=pathlib.Path(td); so=tmp/'writer.so'
    subprocess.run(['cc','-shared','-fPIC','-D_FILE_OFFSET_BITS=64','-o',str(so),str(ROOT/'src/system/tagmanager/tagwrite.c')],check=True)
    lib=ctypes.CDLL(str(so)); write=lib.tagwrite_set; write.argtypes=[ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p]; write.restype=ctypes.c_int
    cover=lib.tagwrite_cover; cover.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int,ctypes.c_int]; cover.restype=ctypes.c_int
    def setcover(path,data):
        buf=ctypes.create_string_buffer(data); return cover(os.fsencode(path),buf,len(data),32,32)
    def run(path,field,value): return write(os.fsencode(path),field,value.encode())
    path=tmp/'song.mp3'; picture=frame(b'APIC',b'\x00image/jpeg\0\x03\0\xff\xd8PICTURE'); title=frame(b'TIT2',b'\x03Title')
    path.write_bytes(id3([picture,title,frame(b'TRCK',b'\x0301/12')]))
    assert run(path,0,'2/12')==0
    frames,audio=parse_id3(path.read_bytes()); assert audio==AUDIO and (b'APIC',picture[10:]) in frames and (b'TIT2',b'\x03Title') in frames and (b'TRCK',b'\x032/12') in frames
    assert run(path,1,'Électro')==0
    frames,audio=parse_id3(path.read_bytes()); assert (b'TCON',b'\x03'+'Électro'.encode()) in frames and audio==AUDIO
    # A chapter/seek frame may contain absolute offsets. Padding keeps the
    # audio stationary; growth in its absence must be refused.
    chap=frame(b'CHAP',b'chapter-offsets')
    path.write_bytes(id3([chap,frame(b'TRCK',b'\x031')]))
    size_before=10+sync(path.read_bytes()[6:10]); assert run(path,0,'2')==0
    assert 10+sync(path.read_bytes()[6:10])==size_before
    data=path.read_bytes(); path.write_bytes(data[:6]+ss(len(chap)+12)+chap+frame(b'TRCK',b'\x032')+AUDIO)
    original=path.read_bytes(); assert run(path,1,'A genre longer than the spare bytes')==2 and path.read_bytes()==original
    path.write_bytes(id3([title],version=3)); assert run(path,1,'Rock')==0
    frames,audio=parse_id3(path.read_bytes()); assert (b'TCON',b'\0Rock') in frames and audio==AUDIO
    assert run(path,1,'é')==0
    frames,audio=parse_id3(path.read_bytes()); assert (b'TCON',b'\x01\xff\xfe'+ 'é'.encode('utf-16le')) in frames and audio==AUDIO
    assert run(path,2,'Tïtle 🎵')==0 and run(path,3,'Björk')==0
    frames,audio=parse_id3(path.read_bytes()); assert (b'TIT2',b'\x01\xff\xfe'+'Tïtle 🎵'.encode('utf-16le')) in frames and (b'TPE1',b'\x01\xff\xfe'+'Björk'.encode('utf-16le')) in frames and audio==AUDIO
    path.write_bytes(id3([frame(b'TRCK',b'\x003',version=3,flags=b'\0\x80')],version=3)); original=path.read_bytes()
    assert run(path,0,'4')==2 and path.read_bytes()==original
    path.write_bytes(id3([title],flags=0x80)); original=path.read_bytes(); assert run(path,1,'Rock')==2 and path.read_bytes()==original
    data=id3([title]); path.write_bytes(data[:10+sync(data[6:10])]+id3([title])); original=path.read_bytes()
    assert run(path,1,'Rock')==2 and path.read_bytes()==original
    # A FLAC with an ID3 prefix must never be treated as an MP3 tag edit.
    prefixed=id3([title]); path.write_bytes(prefixed[:10+sync(prefixed[6:10])]+b'fLaC'+AUDIO)
    original=path.read_bytes(); assert run(path,2,'New')==2 and path.read_bytes()==original
    # Replacing front art must keep the back picture and all audio/frames.
    jpeg=b'\xff\xd8new-jpeg\xff\xd9'
    front=frame(b'APIC',b'\0image/jpeg\0\x03\0old-front')
    back=frame(b'APIC',b'\0image/jpeg\0\x04\0back-cover')
    path.write_bytes(id3([title,front,back])); assert setcover(path,jpeg)==0
    frames,audio=parse_id3(path.read_bytes()); assert audio==AUDIO and (b'APIC',back[10:]) in frames and (b'APIC',b'\0image/jpeg\0\x03\0'+jpeg) in frames and (b'APIC',front[10:]) not in frames
    assert run(path,2,'New title')==0 and run(path,3,'New artist')==0
    frames,audio=parse_id3(path.read_bytes()); assert (b'TIT2',b'\x03New title') in frames and (b'TPE1',b'\x03New artist') in frames and audio==AUDIO
    path.write_bytes(id3([frame(b'APIC',b'broken')]))
    original=path.read_bytes(); assert setcover(path,jpeg)==2 and path.read_bytes()==original
    path.write_bytes(id3([frame(b'APIC',b'\0image/jpeg\0\3\0old',flags=b'\0\x80')]))
    original=path.read_bytes(); assert setcover(path,jpeg)==2 and path.read_bytes()==original
    assert setcover(path,b'not a jpeg')==1 and path.read_bytes()==original
    flac=tmp/'song.flac'; info=b'\x01'*34; pic=b'PICTURE-DATA'
    flac.write_bytes(b'fLaC'+block(0,info)+block(4,vorbis([b'TITLE=Song',b'genre=Old',b'GENRE=Duplicate',b'TRACKNUMBER=3/9']))+block(6,pic,True)+AUDIO)
    assert run(flac,1,'Jazz')==0
    blocks,audio=parse_flac(flac.read_bytes()); assert audio==AUDIO and blocks[0]==(0,info) and blocks[2]==(6,pic) and fields(blocks[1][1])==[b'TITLE=Song',b'TRACKNUMBER=3/9',b'GENRE=Jazz']
    assert run(flac,0,'4/9')==0
    blocks,audio=parse_flac(flac.read_bytes()); assert fields(blocks[1][1])==[b'TITLE=Song',b'GENRE=Jazz',b'TRACKNUMBER=4/9'] and audio==AUDIO
    assert run(flac,2,'Titre')==0 and run(flac,3,'Artiste')==0
    blocks,audio=parse_flac(flac.read_bytes()); assert b'TITLE=Titre' in fields(blocks[1][1]) and b'ARTIST=Artiste' in fields(blocks[1][1]) and blocks[2]==(6,pic) and audio==AUDIO
    frontpic=struct.pack('>I',3)+b'old-front'; backpic=struct.pack('>I',4)+b'back'
    flac.write_bytes(b'fLaC'+block(0,info)+block(6,frontpic)+block(6,backpic,True)+AUDIO)
    assert setcover(flac,jpeg)==0
    blocks,audio=parse_flac(flac.read_bytes()); assert blocks[0]==(0,info) and blocks[1]==(6,backpic) and blocks[2][0]==6 and blocks[2][1][:4]==struct.pack('>I',3) and blocks[2][1].endswith(jpeg) and audio==AUDIO
    flac.write_bytes(b'fLaC'+block(0,info)+block(6,b'bad',True)+AUDIO)
    original=flac.read_bytes(); assert setcover(flac,jpeg)==2 and flac.read_bytes()==original
    flac.write_bytes(b'fLaC'+block(0,info,True)+AUDIO); assert setcover(flac,jpeg)==0
    blocks,audio=parse_flac(flac.read_bytes()); assert blocks[0]==(0,info) and blocks[1][0]==6 and blocks[1][1].endswith(jpeg) and audio==AUDIO
    flac.write_bytes(b'fLaC'+block(0,info,True)+AUDIO); assert run(flac,1,'Rock')==0
    blocks,audio=parse_flac(flac.read_bytes()); assert blocks[0]==(0,info) and fields(blocks[1][1])==[b'GENRE=Rock'] and audio==AUDIO
    flac.write_bytes(b'fLaC'+block(0,info)+block(4,b'\xff'*10,True)+AUDIO); original=flac.read_bytes()
    assert run(flac,1,'Rock')==2 and flac.read_bytes()==original
    assert run(flac,0,'0')==1 and run(flac,0,'3/2')==1 and run(flac,1,'')==1
    link=tmp/'link.flac'; link.symlink_to(flac); assert run(link,1,'Rock')==3 and flac.read_bytes()==original
    flac.chmod(0o444); assert run(flac,1,'Rock')==3 and flac.read_bytes()==original
    assert not list(tmp.glob('*.sonix-*'))
print('tag writer tests passed')
