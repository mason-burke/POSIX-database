# POSIX-database

## overview

This was my last project for my course "Introduction to Computer Systems." The goal of this project was to have a thread-safe BST database that can interact with client threads. This was implemented using the POSIX thread architecture.

## structure

In the server, we begin a listening thread with a port specified by the program argument. This will accept clients to run with the server's database, which has been made thread-safe. Additionally, a handler for SIGINT has been constructed, such that the server will not terminate on SIGINT, and will instead cancel all client threads and continue processing. The server can print out the current state of the database either to a file or stdout, as well as stop and resume client execution. After EOF is given as input to the server, the client threads and listening thread are canceled, and the database is deleted when the server is assured that there are no remaining active clients.

## disclaimers

At the beginning of the project, we were given some completed template code: client.c, client.h, comm.c, comm.h, and db.h. The objective of this project was to create a functional server from scratch in server.c, and take the existing template code from db.c and make it thread-safe.

## how to use
Download the code, then run 

    make clean all 
    
in the root directory. In one terminal window, start a server with 

    ./server <port number>
    
noting that the port number must be 1024 or greater. Once you start a server, you'll enter the server's Read Evaluate Print Loop, or REPL. In this REPL, the available commands are:

    s: pauses all client execution. Often used before adding any clients at the start of a test so that multiple client types can be created.
    g: resumes all client execution.
    p [file]: prints out a database's contents to [file] if provided (w+), alternatively printed to the terminal.
    
In another terminal window, you can start a group of clients with the command
  
    ./client <hostname of server> <port of server> [script] [occurrences]
    
with the last two arguments being optional. Clients run without scripts or occurences will take commands manually. Here are the available client commands:
    
    a <key> <value>: Adds <key> into the database with value <value>.
    q <key>: Retrieves (queries) the value stored with key <key>.
    d <key>: Deletes the given key and its associated value from the database.
    f <file>: Executes the sequence of commands contained in the specified file 
                  (use this to run a script from a single client).
    
Alternatively, running with a script name and an occurence number will create as many clients as specified by occurrences that execute the commands in the script file. These scripts are found in the "scripts" directory. If a client receives EOF from either the command line or the end of the script file, its thread will be canceled by the server.

Included is a file cs0330_db_check, which can be used to verify the results of the database manipulations. You can run 
  
    ./cs0330_db_check <txtfile>

to test a database saved to txtfile.
