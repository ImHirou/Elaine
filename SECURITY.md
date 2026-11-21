# Security

Elaine reads bytes from anyone who can reach its port, and worlds from wherever an operator got
them. A bug in that reading is a vulnerability, and it is reported privately.

## Reporting

Use GitHub's private reporting: **Security → Report a vulnerability** on
<https://github.com/ImHirou/elaine>. Please do not open a public issue or pull request for it, and
do not test against servers that are not yours.

Say what you can of: the commit, how to trigger it (a packet capture, a script for
`tests/integration/mcproto.py`, a world file), and what happens.

## What counts

Anything by which a client, anyone who can connect to the port, or a world given to the importer can

- crash the server or make it stop ticking;
- run code, or read or write memory it should not;
- make the server spend memory, CPU, disk or bandwidth far out of proportion to what it sent;
- act as another player, or as an operator, without being one;
- read or write files outside the world's directory.

## What does not

Behaviour that vanilla has too. Elaine copies the vanilla server, including what one may think it
should not do; if the vanilla server does the same with the same input, it is an ordinary issue
(and a proposal to differ from vanilla, which needs a reason).

## What to expect

Elaine is young and has one maintainer. Only the `master` branch is supported; there are no
releases to backport to yet. Reports are answered as time allows - there is no promised deadline -
and a fix is credited to its reporter unless they ask otherwise.
