#ifndef TERMIOS_H
#define TERMIOS_H
typedef unsigned tcflag_t;
typedef unsigned char cc_t;
typedef unsigned speed_t;
#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
};
#define BRKINT 0000002
#define ICRNL  0000400
#define IXON   0002000
#define OPOST  0000001
#define ONLCR  0000004
#define CS8    0000060
#define CREAD  0000200
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define IEXTEN 0100000
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define B0 0
#define B9600 13
#define B19200 14
#define B38400 15
int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
int tcflush(int fd, int queue_selector);
int cfsetospeed(struct termios *termios_p, speed_t speed);
int cfsetispeed(struct termios *termios_p, speed_t speed);
speed_t cfgetospeed(const struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
#endif
