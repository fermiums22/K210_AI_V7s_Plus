import audio
from Maix import I2S
print("audio dir:", [a for a in dir(audio) if not a.startswith('__')])
print("I2S dir:", [a for a in dir(I2S) if not a.startswith('__')])
try:
    from board import board_info
    print("board_info:", board_info.__dict__ if hasattr(board_info,'__dict__') else board_info)
except Exception as e:
    print("board_info err:", e)
try:
    import Maix
    print("Maix dir:", [a for a in dir(Maix) if not a.startswith('__')])
except Exception as e:
    print("Maix err:", e)
