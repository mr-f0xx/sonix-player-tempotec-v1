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

    # --- M4A (MP4 ilst) ---
    def mbox(typ, payload): return struct.pack('>I', 8+len(payload)) + typ + payload
    def mdata(payload, flag=1): return mbox(b'data', struct.pack('>II', flag, 0) + payload)
    def mitem(typ, payload, flag=1): return mbox(typ, mdata(payload, flag))
    def build_m4a(moov_before=True, with_free=False, genre_gnre=False):
        ilst = b''.join([mitem(b'\xa9nam', b'Old Title'), mitem(b'\xa9ART', b'Old Artist'),
            mbox(b'gnre', mdata(struct.pack('>H', 1), 0)) if genre_gnre else mitem(b'\xa9gen', b'Old Genre'),
            mitem(b'trkn', struct.pack('>HHHH', 0, 3, 9, 0), 0)])
        meta = mbox(b'meta', b'\x00\x00\x00\x00' + mbox(b'hdlr', b'\x00'*24) + mbox(b'ilst', ilst))
        udta = mbox(b'udta', meta)
        ftyp = mbox(b'ftyp', b'M4A \x00\x00\x00\x00M4A ')
        def moov(stco):
            stbl = mbox(b'stbl', mbox(b'stco', b'\x00\x00\x00\x00' + struct.pack('>II', 1, stco)))
            return mbox(b'moov', mbox(b'trak', mbox(b'mdia', mbox(b'minf', stbl))) + udta)
        mdat = mbox(b'mdat', AUDIO)
        freeb = mbox(b'free', b'\x00'*200) if with_free else b''
        if moov_before:
            off = len(ftyp) + len(moov(0)) + len(freeb) + 8
            return ftyp + moov(off) + freeb + mdat
        return ftyp + mdat + moov(len(ftyp) + 8)
    def parse_m4a(data):
        idx = data.find(b'ilst'); start = idx - 4; end = start + struct.unpack('>I', data[start:start+4])[0]
        pos = start + 8; out = {}
        while pos < end:
            sz = struct.unpack('>I', data[pos:pos+4])[0]; typ = data[pos+4:pos+8]
            payload = data[pos+8:pos+sz]; dsz = struct.unpack('>I', payload[0:4])[0]
            assert payload[4:8] == b'data'
            out[typ] = (struct.unpack('>I', payload[8:12])[0], payload[16:dsz]); pos += sz
        return out
    def m4a_stco_ok(data):
        mi = data.find(b'mdat'); si = data.find(b'stco')
        return struct.unpack('>I', data[si+12:si+16])[0] == mi - 4 + 8
    m4a = tmp/'song.m4a'
    m4a.write_bytes(build_m4a()); assert run(m4a, 1, 'Jazz') == 0
    tags = parse_m4a(m4a.read_bytes()); assert tags[b'\xa9gen'][1] == b'Jazz' and tags[b'\xa9nam'][1] == b'Old Title' and AUDIO in m4a.read_bytes() and m4a_stco_ok(m4a.read_bytes())
    m4a.write_bytes(build_m4a(with_free=True)); size0 = len(m4a.read_bytes()); assert run(m4a, 2, 'A much longer title than before') == 0
    assert len(m4a.read_bytes()) == size0 and parse_m4a(m4a.read_bytes())[b'\xa9nam'][1] == b'A much longer title than before' and m4a_stco_ok(m4a.read_bytes())
    m4a.write_bytes(build_m4a()); assert run(m4a, 2, 'A much longer title than before') == 0
    assert parse_m4a(m4a.read_bytes())[b'\xa9nam'][1] == b'A much longer title than before' and m4a_stco_ok(m4a.read_bytes())
    m4a.write_bytes(build_m4a(moov_before=False)); assert run(m4a, 3, 'New Artist') == 0 and parse_m4a(m4a.read_bytes())[b'\xa9ART'][1] == b'New Artist'
    m4a.write_bytes(build_m4a(moov_before=False)); assert run(m4a, 0, '5/12') == 0 and parse_m4a(m4a.read_bytes())[b'trkn'][1] == struct.pack('>HHHH', 0, 5, 12, 0)
    m4a.write_bytes(build_m4a(moov_before=False)); assert setcover(m4a, jpeg) == 0 and parse_m4a(m4a.read_bytes())[b'covr'] == (13, jpeg)
    m4a.write_bytes(build_m4a(moov_before=False, genre_gnre=True)); assert run(m4a, 1, 'Rock') == 0
    tags = parse_m4a(m4a.read_bytes()); assert tags[b'\xa9gen'][1] == b'Rock' and b'gnre' not in tags
    # An MP4 without ilst is refused unchanged.
    bare = mbox(b'ftyp', b'M4A \x00\x00\x00\x00M4A ') + mbox(b'moov', mbox(b'mvhd', b'\x00'*100)) + mbox(b'mdat', AUDIO)
    m4a.write_bytes(bare); original = m4a.read_bytes(); assert run(m4a, 2, 'New') == 2 and m4a.read_bytes() == original
    # --- Ogg Opus / Vorbis ---
    import base64 as _b64
    _crc = []
    for _i in range(256):
        _r = _i << 24
        for _ in range(8): _r = ((_r << 1) ^ 0x04c11db7) & 0xffffffff if _r & 0x80000000 else (_r << 1) & 0xffffffff
        _crc.append(_r)
    def ogg_crc(data):
        c = 0
        for b in data: c = ((c << 8) & 0xffffffff) ^ _crc[((c >> 24) & 0xff) ^ b]
        return c
    def ogg_page(serial, seq, typ, granule, segs, data):
        h = b'OggS' + bytes([0, typ]) + struct.pack('<Q', granule) + struct.pack('<I', serial) + struct.pack('<I', seq) + b'\x00\x00\x00\x00' + bytes([len(segs)]) + bytes(segs) + data
        return h[:22] + struct.pack('<I', ogg_crc(h)) + h[26:]
    def ogg_packets(serial, seq0, typ0, packet):
        pages = []; off = 0; seq = seq0; first = True
        while True:
            left = len(packet) - off; need = left//255 + 1; here = min(need, 255); take = left if here == need else 255*here
            pages.append(ogg_page(serial, seq, typ0 if first else 1, 0, [255]*(here-1) + [take - 255*(here-1)], packet[off:off+take]))
            seq += 1; off += take; first = False
            if off >= len(packet): break
        return pages, seq
    def build_opus(comments, serial=0x1234):
        head = b'OpusHead' + bytes([1, 2]) + struct.pack('<H', 312) + struct.pack('<I', 48000) + struct.pack('<h', 0) + bytes([0])
        tags = b'OpusTags' + vorbis(comments)
        pages, seq = ogg_packets(serial, 0, 2, head); more, seq = ogg_packets(serial, seq, 0, tags); pages += more
        for i in range(2):
            d = b'AUDIO' + bytes([i])*20; pages.append(ogg_page(serial, seq, 4 if i == 1 else 0, (i+1)*960, [len(d)], d)); seq += 1
        return b''.join(pages)
    def parse_opus(data):
        pos = 0; packets = []; cur = b''; pages = []
        while pos < len(data):
            assert data[pos:pos+4] == b'OggS'
            nseg = data[pos+26]; segs = list(data[pos+27:pos+27+nseg]); dlen = sum(segs)
            pages.append(data[pos:pos+27+nseg+dlen]); off = 27+nseg
            for s in segs:
                cur += data[pos+off:pos+off+s]; off += s
                if s < 255: packets.append(cur); cur = b''
            pos += 27+nseg+dlen
        assert cur == b''
        return packets, pages
    opus = tmp/'song.opus'
    opus.write_bytes(build_opus([b'TITLE=Old', b'GENRE=Old', b'TRACKNUMBER=3'])); assert run(opus, 1, 'Jazz') == 0
    packets, pages = parse_opus(opus.read_bytes()); assert packets[0].startswith(b'OpusHead') and packets[1].startswith(b'OpusTags') and b'GENRE=Jazz' in fields(packets[1][8:]) and len(packets) == 4
    opus.write_bytes(build_opus([b'TITLE=Old'])); assert run(opus, 2, 'Nouveau') == 0 and run(opus, 3, 'Artiste') == 0 and run(opus, 0, '4/9') == 0
    packets, _ = parse_opus(opus.read_bytes()); assert b'TITLE=Nouveau' in fields(packets[1][8:]) and b'ARTIST=Artiste' in fields(packets[1][8:]) and b'TRACKNUMBER=4/9' in fields(packets[1][8:])
    opus.write_bytes(build_opus([b'TITLE=Song'])); assert setcover(opus, jpeg) == 0
    packets, _ = parse_opus(opus.read_bytes()); pics = [c for c in fields(packets[1][8:]) if c.startswith(b'METADATA_BLOCK_PICTURE=')]
    assert len(pics) == 1 and jpeg in _b64.b64decode(pics[0].split(b'=', 1)[1])
    opus.write_bytes(build_opus([b'TITLE=Song', b'METADATA_BLOCK_PICTURE=' + _b64.b64encode(b'OLD')])); assert setcover(opus, jpeg) == 0
    packets, _ = parse_opus(opus.read_bytes()); pics = [c for c in fields(packets[1][8:]) if c.startswith(b'METADATA_BLOCK_PICTURE=')]
    assert len(pics) == 1 and jpeg in _b64.b64decode(pics[0].split(b'=', 1)[1])
    big = b'\xff\xd8' + b'X'*100000 + b'\xff\xd9'; opus.write_bytes(build_opus([b'TITLE=A'])); assert setcover(opus, big) == 0
    packets, pages = parse_opus(opus.read_bytes()); assert [struct.unpack('<I', pg[18:22])[0] for pg in pages] == list(range(len(pages)))
    # Vorbis in Ogg shares the comment writer.
    vid = b'\x01vorbis' + b'\x00'*23; vtags = b'\x03vorbis' + vorbis([b'TITLE=Old'])
    pages, seq = ogg_packets(0x9999, 0, 2, vid); more, seq = ogg_packets(0x9999, seq, 0, vtags); pages += more
    pages.append(ogg_page(0x9999, seq, 4, 960, [50], b'AUDIO'*10))
    ogg = tmp/'song.ogg'; ogg.write_bytes(b''.join(pages)); assert run(ogg, 2, 'New') == 0
    packets, _ = parse_opus(ogg.read_bytes()); assert packets[1].startswith(b'\x03vorbis') and b'TITLE=New' in fields(packets[1][7:])
    # An Ogg with an ID3 prefix is refused unchanged.
    opus.write_bytes(id3([title])[:10+sync(id3([title])[6:10])] + build_opus([b'TITLE=Old'])); original = opus.read_bytes()
    assert run(opus, 2, 'New') == 2 and opus.read_bytes() == original
    # ID3-tagged AAC (ADTS) edits through the ID3 writer.
    aac = tmp/'song.aac'; aac.write_bytes(id3([title])); assert run(aac, 2, 'New title') == 0
    frames, audio = parse_id3(aac.read_bytes()); assert (b'TIT2', b'\x03New title') in frames and audio == AUDIO
    assert not list(tmp.glob('*.sonix-*'))
print('tag writer tests passed')
