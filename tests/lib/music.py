"""The stand-in music library both live sets run with (tests/agent/agent_eval.py) and the request loop's offline
test (tests/sim/agent_loop_test.py) uses. Standard library only.
"""
import os

# The music library every case sees (both sets): the artists, titles and genres the cases ask for, plus a
# few others to choose from. Each file holds an ID3v2.3 tag and no sound (ZB_DRY_RUN plays nothing), so a
# run never reads the music of whoever runs it. ZB_EVAL_MUSIC_DIR names another folder; set empty, the
# library is empty, as the runs before 2026-11-19 had.
MUSIC_LIBRARY = [
    ("ABBA", "Gold", "Pop", ["Dancing Queen", "Waterloo", "Mamma Mia"]),
    ("Queen", "A Night at the Opera", "Rock", ["Bohemian Rhapsody", "You're My Best Friend"]),
    ("Queen", "Jazz", "Rock", ["Don't Stop Me Now"]),
    ("The Beatles", "Abbey Road", "Rock", ["Here Comes the Sun", "Come Together"]),
    ("Pink Floyd", "Wish You Were Here", "Rock", ["Wish You Were Here", "Shine On You Crazy Diamond"]),
    ("Fleetwood Mac", "Rumours", "Rock", ["Dreams", "Go Your Own Way"]),
    ("Miles Davis", "Kind of Blue", "Jazz", ["So What", "Blue in Green"]),
    ("Ella Fitzgerald", "Ella Sings Gershwin", "Jazz", ["Summertime", "Someone to Watch Over Me"]),
    ("Brian Eno", "Ambient 1: Music for Airports", "Ambient", ["1/1", "2/1"]),
    ("Ludovico Einaudi", "Una Mattina", "Classical", ["Nuvole Bianche", "Una Mattina"]),
]

def make_music_library(folder):
    def frame(fid, text):
        body = b"\x00" + text.encode("latin-1")
        return fid.encode() + len(body).to_bytes(4, "big") + b"\x00\x00" + body
    for artist, album, genre, titles in MUSIC_LIBRARY:
        d = os.path.join(folder, artist, album)
        os.makedirs(d, exist_ok=True)
        for n, title in enumerate(titles, 1):
            tag = b"".join(frame(f, v) for f, v in (("TIT2", title), ("TPE1", artist), ("TALB", album), ("TCON", genre)))
            size = bytes([(len(tag) >> 21) & 0x7f, (len(tag) >> 14) & 0x7f, (len(tag) >> 7) & 0x7f, len(tag) & 0x7f])
            name = "%02d %s.mp3" % (n, title.replace("/", "-"))
            with open(os.path.join(d, name), "wb") as f:
                f.write(b"ID3\x03\x00\x00" + size + tag)
    return folder
