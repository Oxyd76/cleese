################################################################################
import pyvga
import blit
import buf

ss = buf.sym('sokoscreen')
pyvga.exittext()
pyvga.framebuffer[:len(ss)] = ss

# blit.fill(pyvga.framebuffer,320,0,0,320,200,0)
################################################################################
import isr
import py8042
import keyb

bufchar = None

def kbd_isr():
	ch = keyb.translate_scancode(py8042.get_scancode())
	if ch:
		global bufchar
		bufchar = ch

dir = None
def clk_isr():
	global bufchar
	global dir

	blit.fill(pyvga.framebuffer, 320, 312, 0, 8, 8, (isr.ticker & 15) + 16)

	if py8042.more_squeaks():
		dx = dy = 0
		while py8042.more_squeaks():
			_,dx,dy = py8042.get_squeak()
		if   dx > 10:  dir = 'l'
		elif dy > 10:  dir = 'k'
		elif dx < -10: dir = 'h'
		elif dy < -10: dir = 'j'
	elif dir:
		bufchar = dir; dir = None
	
isr.setvec(clk_isr, kbd_isr)

################################################################################

#--test map--
#map = list('     #####               #   #               #   #             ###   ##            #      #          ### # ## #   ###### #   # ## #####  ..# #   .$          ..# ##### ### #@##  ..#     #     #########     #######')

#--easier level--
map = list('     #####               #   #               #$  #             ###  $##            #  $ $ #          ### # ## #   ###### #   # ## #####  ..# # $  $          ..# ##### ### #@##  ..#     #     #########     #######')

#--harder level--
#map = list('           #######             #  ...#         #####  ...#         #      . .#         #  ##  ...#         ## ##  ...#        ### ########        # $$$ ##        #####  $ $ #####   ##   #$ $   #   #   #@ $  $    $  $ #   ###### $$ $ #####        #      #            ########')

def disptile(off):
	ch = map[off]
	if ch == '@':
	    blit.fill(pyvga.framebuffer, 320,
		(off % 20) << 3, (off / 20) << 3,	# x, y
		8, 8, 1)
	else:
	    blit.fill(pyvga.framebuffer, 320,
		(off % 20) << 3, (off / 20) << 3,	# x, y
		8, 8,					# dx, dy
		{ ' ': 0, '#': 31, '.': 5,		# color
	          '*': 2, '$': 4,
	          '@': 1, '&': 1 }[ch])

def dispall():
	i = len(map)
	eol = 0
	while i > 0:		# no for yet?
		i = i - 1
		if eol and map[i] != ' ':
			eol = 0
		if not eol:
			disptile(i)
		if not (i % 20):
			eol = 1

def move(dir):
	if map.count('@'):	soko = map.index('@')
	else:			soko = map.index('&')

	s = list('~~~')
	s[0] = map[soko]
	s[1] = map[soko+dir]
	s[2] = map[soko+dir+dir]

	if s[1] in ' .':
		s[0] = leave(s[0])
		s[1] = enter(s[1])
	elif s[1] in '$*' and s[2] in ' .':
		s[0] = leave(s[0])
		s[1] = enter(s[1])
		s[2] = slide(s[2])

	map[soko]         = s[0]
	map[soko+dir]     = s[1]
	map[soko+dir+dir] = s[2]

	disptile(soko)
	disptile(soko+dir)
	disptile(soko+dir+dir)

def leave(c):
	if c == '@':	return ' '
        else:        	return '.'

def enter(c):
	if c in ' $':	return '@'
	else:		return '&'

def slide(c):
	if c == ' ':	return '$'
        else:        	return '*'

dispall()
while 1:
	def loop(msg):
		pyvga.cleartext()
		pyvga.entertext()
		print msg
		while 1: pass

	if not map.count('$'):
		loop('You Win!')

	bufchar = None
	while not bufchar:
		pass

	if   bufchar == 'q':	loop('Thanks for playing')
	elif bufchar == 'h':	move(-1)
	elif bufchar == 'j':	move(20)
	elif bufchar == 'k':	move(-20)
	elif bufchar == 'l':	move(1)
	elif bufchar == 'p':	dispall()
