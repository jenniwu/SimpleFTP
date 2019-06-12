# SimpleFTP

A command line FTP client written in C. The client supports the following commands as well as a variety of error messages:

- user [username] Provides the FTP server a username.
- pw [password] Provides the FTP server a password.
- dir Opens a new data TCP connection and lists the contents of the current directory.
- cd [directoryName] Navigates to the given directoryName.
- get [fileName] Opens a new data TCP connection and retrieves the file named fileName.
- quit Closes the current FTP session.

To run
`make run` (or alternatively to use another FTP server: `make`; then: `java -jar CSftp.jar [ftpServerName] 21`)
