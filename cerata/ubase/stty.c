/* See LICENSE file for copyright and license details. */
#include <sys/ioctl.h>
#include <sys/ttydefaults.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "util.h"

/*
 * Petty POSIX violations:
 * 
 * -  XBD 12.2 is not honoured precisely. This is for
 *    convenience and compatibility with other implementations.
 */

#define CC_MAX 255

static int output_size_requested = 0;
static int output_speed_requested = 0;
static int drain_requested = 1;

static void sane(int, struct termios *);
static void setwinsize(long, long);
static void ispeed(char *, struct termios *);
static void ospeed(char *, struct termios *);

static void
raw(int unset, struct termios *m)
{
	if (!unset) {
		m->c_iflag = 0;
#ifdef XCASE
		m->c_lflag &= ~XCASE;
#endif
		m->c_cc[VMIN] = 1;
		m->c_cc[VTIME] = 0;
	} else {
		m->c_iflag |= BRKINT | IGNPAR | ISTRIP | ICRNL | IXON;
	}
}

static void
evenp(int unset, struct termios *m)
{
	m->c_oflag &= ~CSIZE;
	m->c_oflag &= ~(unset ? PARENB : PARODD);
	m->c_oflag |= unset ? CS8 : (CS7 | PARENB);
}


static void
dec(int unset, struct termios *m)
{
	m->c_cc[VINTR] = CINTR;
	m->c_cc[VKILL] = CKILL;
	m->c_cc[VERASE] = CERASE;
	(void) unset;
}

static void
ek(int unset, struct termios *m)
{
	m->c_cc[VKILL] = CKILL;
	m->c_cc[VERASE] = CERASE;
	(void) unset;
}

static void
nl(int unset, struct termios *m)
{
	if (unset) {
		m->c_iflag &= ~(INLCR | IGNCR);
		m->c_oflag &= ~(OCRNL | ONLRET);
	}
}

static void
oddp(int unset, struct termios *m)
{
	m->c_oflag &= ~CSIZE;
	m->c_oflag &= ~(unset ? PARENB : 0);
	m->c_oflag |= unset ? CS8 : (CS7 | PARODD | PARENB);
}

static void drain(int unset, struct termios *m)  { drain_requested = !unset; (void) m; }
static void cooked(int unset, struct termios *m) { raw(!unset, m); }
static void pass8(int unset, struct termios *m)  { m->c_cflag &= ~CSIZE, m->c_cflag |= unset ? CS7 : CS8; }
static void size(int unset, struct termios *m)   { output_size_requested = 1; (void) m; (void) unset; }
static void speed(int unset, struct termios *m)  { output_speed_requested = 1; (void) m; (void) unset; }
static void tabs(int unset, struct termios *m)   { m->c_oflag &= ~TABDLY, m->c_oflag |= unset ? TAB3 : TAB0; }
static void cols(char *arg, struct termios *m)   { setwinsize(-1, estrtonum(arg, 0, USHRT_MAX)); (void) m; }
static void min(char *arg, struct termios *m)    { m->c_cc[VMIN] = estrtonum(arg, 0, CC_MAX); }
static void rows(char *arg, struct termios *m)   { setwinsize(estrtonum(arg, 0, USHRT_MAX), -1); (void) m; }
static void stime(char *arg, struct termios *m)  { m->c_cc[VTIME] = estrtonum(arg, 0, CC_MAX); }

enum type { CTRL, IN, OUT, LOCAL, COMB, SPEC };
enum {
	BOOL = 1,
	DUP = 2,
	SANE = 4,
	INSANE = 8,
	CBREAK = 16,
	DECCTLQ = 32,
	LCASE = 64,
	PASS8 = 128,
	LITOUT = 256,
	CRT = 1024,
	DEC = 2048,
	NL = 4096,
	COOKED = 8192,
	DEF = 16384
};

struct mode {
	const char *op;
	enum type type;
	tcflag_t set;
	tcflag_t clear;
	void (*fun)(int, struct termios *);
	int flags;
};

struct key {
	const char *op;
	size_t index;
	cc_t sanevalue;
};

struct intvalued {
	const char *op;
	void (*fun)(char *, struct termios *);
};

struct speed {
	const char *str;
	speed_t speed;
};

struct line {
	const char *str;
	unsigned char value;
};

static const struct mode modes[] = {
	{"clocal",   CTRL,  CLOCAL,  0,       0,      BOOL},
#ifdef CMSPAR
	{"cmspar",   CTRL,  CMSPAR,  0,       0,      BOOL},
#endif
	{"cread",    CTRL,  CREAD,   0,       0,      BOOL | SANE},
	{"crtscts",  CTRL,  CRTSCTS, 0,       0,      BOOL},
	{"cs5",      CTRL,  CS5,     CSIZE,   0,      0},
	{"cs6",      CTRL,  CS6,     CSIZE,   0,      0},
	{"cs7",      CTRL,  CS7,     CSIZE,   0,      0},
	{"cs8",      CTRL,  CS8,     CSIZE,   0,      DEF},
	{"cstopb",   CTRL,  CSTOPB,  0,       0,      BOOL},
	{"hup",      CTRL,  HUPCL,   0,       0,      BOOL | DUP},
	{"hupcl",    CTRL,  HUPCL,   0,       0,      BOOL | DEF},
	{"parenb",   CTRL,  PARENB,  0,       0,      BOOL | PASS8 | LITOUT},
	{"parodd",   CTRL,  PARODD,  0,       0,      BOOL},

	{"brkint",   IN,    BRKINT,  0,       0,      BOOL | SANE},
	{"icrnl",    IN,    ICRNL,   0,       0,      BOOL | SANE | NL},
	{"ignbrk",   IN,    IGNBRK,  0,       0,      BOOL | INSANE},
	{"igncr",    IN,    IGNCR,   0,       0,      BOOL | INSANE},
	{"ignpar",   IN,    IGNPAR,  0,       0,      BOOL},
	{"imaxbel",  IN,    IMAXBEL, 0,       0,      BOOL | SANE},
	{"inlcr",    IN,    INLCR,   0,       0,      BOOL | INSANE},
	{"inpck",    IN,    INPCK,   0,       0,      BOOL},
	{"istrip",   IN,    ISTRIP,  0,       0,      BOOL | PASS8 | LITOUT},
	{"iuclc",    IN,    IUCLC,   0,       0,      BOOL | INSANE | LCASE},
	{"iutf8",    IN,    IUTF8,   0,       0,      BOOL | SANE},
	{"ixany",    IN,    IXANY,   0,       0,      BOOL | INSANE | DECCTLQ},
	{"ixoff",    IN,    IXOFF,   0,       0,      BOOL | INSANE},
	{"ixon",     IN,    IXON,    0,       0,      BOOL | DEF},
	{"parmrk",   IN,    PARMRK,  0,       0,      BOOL},
	{"tandem",   IN,    IXOFF,   0,       0,      BOOL | DUP},

	{"bs0",      OUT,   BS0,     BSDLY,   0,      SANE},
	{"bs1",      OUT,   BS1,     BSDLY,   0,      INSANE},
	{"cr0",      OUT,   CR0,     CRDLY,   0,      SANE},
	{"cr1",      OUT,   CR1,     CRDLY,   0,      INSANE},
	{"cr2",      OUT,   CR2,     CRDLY,   0,      INSANE},
	{"cr3",      OUT,   CR3,     CRDLY,   0,      INSANE},
	{"ff0",      OUT,   FF0,     FFDLY,   0,      SANE},
	{"ff1",      OUT,   FF1,     FFDLY,   0,      INSANE},
	{"nl0",      OUT,   NL0,     NLDLY,   0,      SANE},
	{"nl1",      OUT,   NL1,     NLDLY,   0,      INSANE},
	{"ocrnl",    OUT,   OCRNL,   0,       0,      BOOL | INSANE},
	{"ofdel",    OUT,   OFDEL,   0,       0,      BOOL | INSANE},
	{"ofill",    OUT,   OFILL,   0,       0,      BOOL | INSANE},
	{"olcuc",    OUT,   OLCUC,   0,       0,      BOOL | INSANE | LCASE},
	{"onlcr",    OUT,   ONLCR,   0,       0,      BOOL | SANE | NL},
	{"onlret",   OUT,   ONLRET,  0,       0,      BOOL | INSANE},
	{"onocr",    OUT,   ONOCR,   0,       0,      BOOL | INSANE},
	{"opost",    OUT,   OPOST,   0,       0,      BOOL | SANE | LITOUT | COOKED},
	{"tab0",     OUT,   TAB0,    TABDLY,  0,      SANE},
	{"tab1",     OUT,   TAB1,    TABDLY,  0,      INSANE},
	{"tab2",     OUT,   TAB2,    TABDLY,  0,      INSANE},
	{"tab3",     OUT,   TAB3,    TABDLY,  0,      INSANE},
	{"vt0",      OUT,   VT0,     VTDLY,   0,      SANE},
	{"vt1",      OUT,   VT1,     VTDLY,   0,      INSANE},

	{"crterase", LOCAL, ECHOE,   0,       0,      BOOL | DUP},
	{"crtkill",  LOCAL, ECHOKE,  0,       0,      BOOL | DUP},
	{"ctlecho",  LOCAL, ECHOCTL, 0,       0,      BOOL | DUP},
	{"echo",     LOCAL, ECHO,    0,       0,      BOOL | SANE},
	{"echoctl",  LOCAL, ECHOCTL, 0,       0,      BOOL | SANE | CRT | DEC},
	{"echoe",    LOCAL, ECHOE,   0,       0,      BOOL | SANE | CRT | DEC},
	{"echok",    LOCAL, ECHOK,   0,       0,      BOOL | SANE},
	{"echoke",   LOCAL, ECHOKE,  0,       0,      BOOL | SANE | CRT | DEC},
	{"echonl",   LOCAL, ECHONL,  0,       0,      BOOL | INSANE},
	{"echoprt",  LOCAL, ECHOPRT, 0,       0,      BOOL | INSANE},
	{"extproc",  LOCAL, EXTPROC, 0,       0,      BOOL | INSANE},
	{"flusho",   LOCAL, FLUSHO,  0,       0,      BOOL | INSANE},
	{"icanon",   LOCAL, ICANON,  0,       0,      BOOL | SANE | CBREAK | COOKED},
	{"iexten",   LOCAL, IEXTEN,  0,       0,      BOOL | SANE},
	{"isig",     LOCAL, ISIG,    0,       0,      BOOL | SANE | COOKED},
	{"noflsh",   LOCAL, NOFLSH,  0,       0,      BOOL | INSANE},
	{"prterase", LOCAL, ECHOPRT, 0,       0,      BOOL | DUP},
	{"tostop",   LOCAL, TOSTOP,  0,       0,      BOOL | INSANE},
#ifdef XCASE
	{"xcase",    LOCAL, XCASE,   0,       0,      BOOL | INSANE | LCASE},
#endif

	{"cbreak",   COMB,  0,       CBREAK,  0,      BOOL | DUP},
	{"cooked",   COMB,  COOKED,  0,       cooked, BOOL | DUP},
	{"crt",      COMB,  CRT,     0,       0,      DUP},
	{"dec",      COMB,  DEC,     DECCTLQ, dec,    DUP},
	{"decctlq",  COMB,  0,       DECCTLQ, 0,      BOOL | DUP},
	{"ek",       COMB,  0,       0,       ek,     DUP},
	{"evenp",    COMB,  0,       0,       evenp,  BOOL | DUP},
	{"LCASE",    COMB,  LCASE,   0,       0,      BOOL | DUP},
	{"lcase",    COMB,  LCASE,   0,       0,      BOOL | DUP},
	{"litout",   COMB,  0,       LITOUT,  pass8,  BOOL | DUP},
	{"nl",       COMB,  0,       NL,      nl,     BOOL | DUP},
	{"oddp",     COMB,  0,       0,       oddp,   BOOL | DUP},
	{"parity",   COMB,  0,       0,       evenp,  BOOL | DUP},
	{"pass8",    COMB,  0,       PASS8,   pass8,  BOOL | DUP},
	{"raw",      COMB,  0,       COOKED,  raw,    BOOL | DUP},
	{"sane",     COMB,  SANE,    INSANE,  sane,   DUP},
	{"tabs",     COMB,  0,       0,       tabs,   BOOL | DUP},

	{"size",     SPEC,  0,       0,       size,   DUP},
	{"speed",    SPEC,  0,       0,       speed,  DUP},
	{"drain",    SPEC,  0,       0,       drain,  BOOL | DUP},

	{0, 0, 0, 0, 0, 0}
};

static const struct key keys[] = {
	{"discard", VDISCARD, CDISCARD},
	{"eof",     VEOF,     CEOF},
	{"eol",     VEOL,     CEOL},
	{"eol2",    VEOL2,    _POSIX_VDISABLE},
	{"erase",   VERASE,   CERASE},
	{"intr",    VINTR,    CINTR},
	{"kill",    VKILL,    CKILL},
	{"lnext",   VLNEXT,   CLNEXT},
	{"quit",    VQUIT,    CQUIT},
	{"rprnt",   VREPRINT, CRPRNT},
	{"start",   VSTART,   CSTART},
	{"stop",    VSTOP,    CSTOP},
	{"susp",    VSUSP,    CSUSP},
	{"swtch",   VSWTC,    _POSIX_VDISABLE},
	{"werase",  VWERASE,  CWERASE},
	{0, 0, 0}
};

static const struct intvalued ints[] = {
	{"cols",    cols},
	{"columns", cols},
	{"min",     min},
	{"rows",    rows},
	{"time",    stime},
	{"ispeed",  ispeed},
	{"ospeed",  ospeed},
	{0, 0}
};

#define B(baud) {#baud, B##baud}
static const struct speed speeds[] = {
	B(0),       B(50),      B(75),      B(110),     B(134),     B(150),     B(200),     B(300),
	B(600),     B(1200),    B(1800),    B(2400),    B(4800),    B(9600),    B(19200),   B(38400),
	B(57600),   B(115200),  B(230400),  B(460800),  B(500000),  B(576000),  B(921600),  B(1000000),
	B(1152000), B(1500000), B(2000000), B(2500000), B(3000000), B(3500000), B(4000000),
	{"134.5", B134},
	{"exta",  B19200},
	{"extb",  B38400},
	{0, 0}
};
#undef B

static const struct line lines[] = {
	{"tty",      N_TTY},
	{"slip",     N_SLIP},
	{"mouse",    N_MOUSE},
	{"ppp",      N_PPP},
	{"strip",    N_STRIP},
	{"ax25",     N_AX25},
	{"x25",      N_X25},
	{"6pack",    N_6PACK},
	{"masc",     N_MASC},
	{"r3964",    N_R3964},
	{"profibus", N_PROFIBUS_FDL},
	{"irda",     N_IRDA},
	{"smsblock", N_SMSBLOCK},
	{"hdlc",     N_HDLC},
	{"syncppp",  N_SYNC_PPP},
	{"hci",      N_HCI},
	{0, 0}
};

static void
sane(int unset, struct termios *m)
{
	const struct key *op = keys;
	for (; op->op; op++)
		m->c_cc[op->index] = op->sanevalue;
	m->c_cc[VMIN] = 1;
	m->c_cc[VTIME] = 0;
	(void) unset;
}

static int
isxnumber(char* str)
{
	if (!*str)
		return 0;
	for (; *str; str++)
		if (!isxdigit(*str))
			return 0;
	return 1;
}

static void
decodehex(char *dest, char* src)
{
	while (*src) {
		char hi = *src++;
		char lo = *src++;
		hi = (hi & 15) + 9 * !isdigit(hi);
		lo = (lo & 15) + 9 * !isdigit(lo);
		*dest++ = (hi << 4) | lo;
	}
}

static void
setwinsize(long y, long x)
{
	struct winsize winsize;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize))
		eprintf("TIOCGWINSZ <stdin>:");
	if (y >= 0)
		winsize.ws_row = y;
	if (x >= 0)
		winsize.ws_col = x;
	if (ioctl(STDIN_FILENO, TIOCSWINSZ, &winsize))
		eprintf("TIOCSWINSZ <stdin>:");
}

static void
setoperand_mode(int unset, const struct mode *op, struct termios *mode)
{
	tcflag_t *bitsp = 0;

	switch (op->type) {
	case CTRL:  bitsp = &mode->c_cflag; break;
	case IN:    bitsp = &mode->c_iflag; break;
	case OUT:   bitsp = &mode->c_oflag; break;
	case LOCAL: bitsp = &mode->c_lflag; break;
	case SPEC:  break;
	default:    abort();
	}

	if (bitsp) {
		*bitsp &= ~op->clear;
		if (!unset)
			*bitsp |= op->set;
		else
			*bitsp &= ~op->set;
	}

	if (op->fun)
		op->fun(unset, mode);
}

static int
parseoperand_mode(char *arg, struct termios *mode)
{
	const struct mode *op = modes;
	const struct mode *op_proper;
	int unset = *arg == '-';
	int flags_set, flags_unset;

	arg += unset;
	while (op->op && strcmp(arg, op->op))
		op++;
	if (!op->op)
		return -1;
	if (unset && !(op->flags & BOOL))
		return -1;

	switch (op->type) {
	case CTRL:
	case IN:
	case OUT:
	case LOCAL:
	case SPEC:
		setoperand_mode(unset, op, mode);
		return 0;
	case COMB:
		break;
	default:
		abort();
	}

	flags_set = (int)(op->set);
	flags_unset = (int)(op->clear);
	op_proper = op;

	if (flags_unset || flags_set) {
		for (op = modes; op->op; op++) {
			if (op->type == COMB)
				continue;
			if (flags_unset && (op->flags & flags_unset))
				setoperand_mode(!unset, op, mode);
			if (flags_set && (op->flags & flags_set))
				setoperand_mode(unset, op, mode);
		}
	}

	if (op_proper->fun)
		op_proper->fun(unset, mode);

	return 0;
}

static long long
estrtonum_anyradix(const char *numstr, long long minval, long long maxval)
{
	long long ll = 0;
	char *ep;
	errno = 0;
	ll = strtoll(numstr, &ep, 0);
	if (numstr == ep || *ep != '\0')
		eprintf("strtoll %s: invalid\n", numstr);
	else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
		eprintf("strtoll %s: too small\n", numstr);
	else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
		eprintf("strtoll %s: too large\n", numstr);
	return ll;
}

static int
parseoperand_key(char *arg0, char *arg1, struct termios *mode)
{
	const struct key *op = keys;
	cc_t value;

	while (op->op && strcmp(arg0, op->op))
		op++;
	if (!op->op)
		return -1;

	if (!arg1)
		eprintf("missing argument for operand: %s\n", arg0);

	if (!strcmp(arg1, "^-") || !strcmp(arg1, "undef"))
		value = _POSIX_VDISABLE;
	else if (!strcmp(arg1, "^?"))
		value = 127;
	else if (!arg1[0] || !arg1[1])
		value = arg1[0];
	else if (arg1[0] == '^')
		value = (cc_t)(arg1[1]) & ~0x60;
	else
		value = estrtonum_anyradix(arg1, 0, CC_MAX);

	mode->c_cc[op->index] = value;
	return 0;
}

static int
parseoperand_int(char *arg0, char *arg1, struct termios *mode)
{
	const struct intvalued *op = ints;

	while (op->op && strcmp(arg0, op->op))
		op++;
	if (!op->op)
		return -1;

	if (!arg1)
		eprintf("missing argument for operand: %s\n", arg0);

	op->fun(arg1, mode);
	return 0;
}

static const char *
baudtostr(speed_t baud)
{
	const struct speed *speed = speeds;
	while (speed->str && speed->speed != baud)
		speed++;
	return speed->str ? speed->str : "0";
}

static const char*
linetostr(unsigned value)
{
	const struct line *ln = lines;
	while (ln->str && ln->value != value)
		ln++;
	return ln->str;
}

static void
line(char *arg, struct termios *m)
{
	const struct line *ln = lines;
	while (ln->str && strcmp(ln->str, arg))
		ln++;
	if (ln->str)
		m->c_line = ln->value;
	else
		m->c_line = estrtonum(arg, 0, 255);
}

static int
parsespeed(char *arg, struct speed *ret)
{
	const struct speed *speed = speeds;
	while (speed->str && strcmp(arg, speed->str))
		speed++;
	if (!speed->str)
		return -1;
	*ret = *speed;
	return 0;
}

static void
eparsespeed(char *arg, struct speed *ret)
{
	if (parsespeed(arg, ret))
		eprintf("invalid speed parameter: %s\n", arg);
}

static void
ispeed(char *arg, struct termios *m)
{
	struct speed speed;
	eparsespeed(arg, &speed);
	if (cfsetispeed(m, speed.speed))
		eprintf("cfsetispeed %s:", speed.str);
}

static void
ospeed(char *arg, struct termios *m)
{
	struct speed speed;
	eparsespeed(arg, &speed);
	if (cfsetospeed(m, speed.speed))
		eprintf("cfsetospeed %s:", speed.str);
}

static void
printtoken(const char *fmt, ...)
{
	static size_t width = 0;
	static size_t pos = 0;
	static char buf[BUFSIZ];
	va_list ap;
	int len;

	if (!width) {
		struct winsize winsize;
		if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize))
			if (winsize.ws_col > 40)
				width = winsize.ws_col;
		if (!width)
			width = SIZE_MAX;
	}

	if (!strcmp(fmt, "\n")) {
		if (pos)
			printf("\n");
		pos = 0;
		return;
	}

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len < 0 || (size_t)len >= sizeof(buf))
		eprintf("vsnprintf:");

	if (pos + !!pos + len > width) {
		printf("\n");
		pos = 0;
	} else if (pos) {
		printf(" ");
		pos++;
	}

	printf("%s", buf);
	pos += len;
}

static const char*
keytostr(cc_t key)
{
	static char buf[5];
	int r;
	if (key == _POSIX_VDISABLE)
		return "undef";
	else if (key < (cc_t)' ')
		r = snprintf(buf, sizeof(buf), "^%c", key + '@');
	else if (key < 127)
		r = snprintf(buf, sizeof(buf), "%c", key);
	else if (key == 127)
		r = snprintf(buf, sizeof(buf), "^?");
	else if (key < 128 + ' ')
		r = snprintf(buf, sizeof(buf), "M-^%c", key - 128 + '@');
	else if (key == 128 + 127)
		r = snprintf(buf, sizeof(buf), "M-^?");
	else
		r = snprintf(buf, sizeof(buf), "M-%c", key - 128);
	if (r < 0 || (size_t)r >= sizeof(buf))
		eprintf("snprintf:");
	return buf;
}

static int
isdefault(int flags)
{
	if (flags & (SANE | INSANE))
		return (flags & SANE) || !(flags & INSANE);
	return flags & DEF;
}

static void
displaysettings(struct termios *m, int all)
{
	const struct key *kbd = keys;
	const struct mode *mod = modes;
	struct winsize winsize;
	speed_t in, out;
	tcflag_t *bitsp, mask;
	const char *linestr;

	in = cfgetispeed(m);
	out = cfgetospeed(m);
	if (!in || in == out) {
		if (all || out != B38400)
			printtoken("speed %s baud;", baudtostr(out));
	} else {
		printtoken("ispeed %s baud;", baudtostr(in));
		printtoken("ospeed %s baud;", baudtostr(out));
	}

	if (all) {
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize))
			eprintf("TIOCGWINSZ <stdin>:");
		printtoken("rows %u;", winsize.ws_row);
		printtoken("columns %u;", winsize.ws_col);
	}
	printtoken("\n");

	if (all || m->c_line != 0) {
		linestr = linetostr(m->c_line);
		if (linestr)
			printtoken("line = %s;", linestr);
		else
			printtoken("line = %u;", (unsigned)(m->c_line));
	}
	if (all || (m->c_cc[VMIN] != 1 && !(m->c_lflag & ICANON)))
		printtoken("min = %u;", (unsigned)(m->c_cc[VMIN]));
	if (all || (m->c_cc[VTIME] != 0 && !(m->c_lflag & ICANON)))
		printtoken("time = %u;", (unsigned)(m->c_cc[VTIME]));
	printtoken("\n");

	for (; kbd->op; kbd++)
		if (all || m->c_cc[kbd->index] != kbd->sanevalue)
			printtoken("%s = %s;", kbd->op, keytostr(m->c_cc[kbd->index]));
	printtoken("\n");

	for (; mod->op; mod++) {
		switch (mod->type) {
		case CTRL:  bitsp = &m->c_cflag; break;
		case IN:    bitsp = &m->c_iflag; break;
		case OUT:   bitsp = &m->c_oflag; break;
		case LOCAL: bitsp = &m->c_lflag; break;
		default:    bitsp = 0;           break;
		}
		if (!bitsp || (mod->flags & DUP))
			continue;
		mask = mod->clear ? mod->clear : mod->set;
		if ((*bitsp & mask) == mod->set) {
			if (all || !isdefault(mod->flags))
				printtoken("%s", mod->op);
		}
		else if (mod->flags & BOOL) {
			if (all || isdefault(mod->flags))
				printtoken("-%s", mod->op);
		}
	}
	printtoken("\n");
}

static void
usage(void)
{
	eprintf("usage: %s [-a | -g] [operand ...]\n", argv0);
}

int
main(int argc, char *argv[])
{
	struct termios mode;
	struct termios mode2;
	struct winsize winsize;
	struct speed speed;
	int aflag = 0;
	int gflag = 0;
	size_t n;
	unsigned char *buf;
	char *p;
	speed_t in, out;

	for (argv0 = *argv++, argc--; argc; argv++, argc--) {
		if (!strcmp(*argv, "-ag") || !strcmp(*argv, "-ga")) {
			aflag = gflag = 1;
		} else if (!strcmp(*argv, "-g")) {
			gflag = 1;
		} else if (!strcmp(*argv, "-a")) {
			aflag = 1;
		} else if (!strcmp(*argv, "--")) {
			argv++, argc--;
			break;
		} else {
			break;
		}
	}

	if (aflag && gflag)
		usage();

	memset(&mode, 0, sizeof(mode));
	if (tcgetattr(STDIN_FILENO, &mode))
		eprintf("tcgetattr <stdin>:");
	memcpy(&mode2, &mode, sizeof(mode));

	for (; *argv; argv++) {
		if (**argv == '=') {
			p = *argv + 1;
			if (strlen(p) != sizeof(mode) * 2 || !isxnumber(p))
				goto invalid;
			decodehex((char *)&mode, p);
		} else if (!parseoperand_mode(*argv, &mode)) {
			/* do nothing. */
		} else if (!parseoperand_key(argv[0], argv[1], &mode)) {
			argv++;
		} else if (!parseoperand_int(argv[0], argv[1], &mode)) {
			argv++;
		} else if (!strcmp(argv[0], "line")) {
			if (!argv[1])
				eprintf("missing argument for operand: %s\n", argv[0]);
			line(argv[1], &mode);
			argv++;
		} else if (!parsespeed(*argv, &speed)) {
			if (cfsetispeed(&mode, speed.speed))
				eprintf("cfsetispeed %s:", speed.str);
			if (cfsetospeed(&mode, speed.speed))
				eprintf("cfsetospeed %s:", speed.str);
		} else {
			goto invalid;
		}
	}

	if (memcmp(&mode, &mode2, sizeof(mode))) {
		memset(&mode2, 0, sizeof(mode2));
		if (tcsetattr(STDIN_FILENO, drain_requested ? TCSADRAIN : TCSANOW, &mode))
			eprintf("tcsetattr <stdin>:");
		if (tcgetattr(STDIN_FILENO, &mode2))
			eprintf("tcgetattr <stdin>:");
		if (memcmp(&mode, &mode2, sizeof(mode)))
			eprintf("tcsetattr <stdin>: unable to apply all operands\n");
	}

	if (gflag) {
		buf = (unsigned char *)&mode;
		printf("=");
		for (n = sizeof(mode); n--; buf++)
			printf("%02x", *buf);
		printf("\n");
	}

	if (output_size_requested) {
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize))
			eprintf("TIOCGWINSZ <stdin>:");
		printf("%u %u\n", winsize.ws_row, winsize.ws_col);
	}

	if (output_speed_requested) {
		in = cfgetispeed(&mode);
		out = cfgetospeed(&mode);
		if (!in || in == out)
			printf("%s\n", baudtostr(out));
		else
			printf("%s %s\n", baudtostr(in), baudtostr(out));
	}

	if ((aflag || !argc) && !gflag)
		displaysettings(&mode, aflag);

	return 0;

invalid:
	eprintf("invalid operand: %s\n", *argv);
}
