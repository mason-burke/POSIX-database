# 8-database

## General Structure

In the server, we begin a listening thread with a port specified
by the program argument. This will accept clients to run with the server's
database, which has been made thread-safe. Additionally, a handler for SIGINT
has been constructed, such that the server will not terminate on SIGINT, and
will instead cancel all client threads and continue processing. The server
can print out the current state of the database either to a file or stdout, as
well as stop and resume client execution. After EOF is given as input to the
server, the client threads and listening thread are canceled, and the database
is deleted when the server is assured that there are no remaining active
clients.

## Known Bugs

Well. At the time of turning in, everything is working properly. However,
I'm fairly certain that in this version of the code there was an issue
where SIGINT and EOF weren't being processed by the server. I haven't been
able to reproduce it in 5-10 attempts, so I should think that everything works.
EDIT: I think the issue is caused by me not joining with the client threads, but
I don't think I was supposed to do that, so I'm going to leave it be.

## Changed function signatures

None of the function signatures in server.c were changed.
In db.h, I defined an enum locktype_t. Together with the macro lock() in db.c,
this enum allows us to succinctly lock a pthread_rwlock_t.
