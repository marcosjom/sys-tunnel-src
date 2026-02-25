
This repository was moved to Github. My original repository has >4 years of activity up to 2023.

# sys-tunnel-src

This is a TCP-tunnel for encryption and obfuscation.

Created by [Marcos Ortega](https://mortegam.com/). Built on top of [sys-nbframework-src](https://github.com/marcosjom/sys-nbframework-src).

# Features

- TCP support.
- SSL/TLS encryption layer.
- Client and/or server side certificate validation.
- 8 bits masking.
- compiled for Windows, Mac, Linux.
- works as a program, a service/daemon or as a dll to be used by apps.

# How to use

On your client:

Run an instance listening to a port, applying one or more layers of conversion and redirecting to your server's port.

On your server:

Run an instance listening to a port, inverting the layers defining on the clients and redirecting to your internal service's port.

Both your client and service will communicate as if the traffic is plain and unencrypted, but the tunnel acts as an intermediary peer-to-peer encryption layer. Any attempt to communicate without a tunnel or a valid tunnel configuration will produce an invalida stream-of-octets at the server side.

# How to compile

For simplicity, create this folder structure:

- my_folder
   - sys-tunnel<br/>
      - [sys-tunnel-src](https://github.com/marcosjom/sys-tunnel-src)<br/>
   - sys-nbframework<br/>
      - [sys-nbframework-src](https://github.com/marcosjom/sys-nbframework-src)<br/>

You can create your own folders structure but it will require to update some paths in the projects and scripts.

Follow the instructions in the `sys-nbframework-src/ext/*_howToBuild.txt` files to download the source of third-party embedded libraries. Optionally, these libraries can be dynamically linked to the ones installed in the operating system.

The following steps will create an executable file.

## Windows

Open `projects/visual-studio/sys-tunnel.sln` and compile the desired target.

## MacOS

Open `projects/xcode/sys-tunnel.xcworkspace` and compile the desired target.

## Linux and Others

In a terminal:

```
cd sys-tunnel-src
make tunnel-server
```

or

```
cd sys-tunnel-src
make tunnel-server NB_LIB_SSL_SYSTEM=1 NB_LIB_LZ4_SYSTEM=1 NB_LIB_Z_SYSTEM=1
```

The first `make` command will embed the dependencies into the executable from its source. The second will link to the libraries installed on the current system.

Check each project's `Makefile` and `MakefileProject.mk` files, and the [MakefileFuncs.mk](https://github.com/marcosjom/makefile-like-IDE) to understand the `make` process, including the accepted flags and targets.

# How to run

```
./tunnel-server [params]

-cfgStart ... -cfgEnd, config-params (see below).
-secsRunAndExit [secs], stop execution after [secs].
-maxSecsWithoutConn [secs], stop execution after [secs] with no conns.
-maxConnsAndExit [amm], stop receiving new conns after [amm] and exits after closed.
-printArgs, prints parsed/loaded args.
-printCfg, prints parsed/loaded cfg-args.
-help | --help, prints help.

Config params:
-cfgStart, following params are parsed as config untill -cfgEnd appears.
-CAs, following params are applied to the default certificate-authorities-list.
 | -path [path], adds one CA certificate to the default list.
 | -pay64 [base64], adds one CA certificate to the default list.
-port [number], adds an in-port to the list, following params are applied to this port.
 | -layer [mask|ssl|base64], adds a layer to the inStack.
 | -ssl, following params are applied to inSsl context.
 |  | -cert, following params are applied to in-ssl-certificate.
 |  |  | -isRequested [0|1], port will request clients to provide an optional certificate.
 |  |  | -isRequired [0|1], port will drop connection if certificate was not provided.
 |  |  | -source, following params are applied to inSsl-certificate-source.
 |  |  |  | -path [path], defines the path for the certificate to use.
 |  |  |  | -pay64 [base64], defines the payload for the certificate to use.
 |  |  |  | -key, following params are applied to inSsl-key.
 |  |  |  |  | -path [path], defines the path of the inSsl-key-file.
 |  |  |  |  | -pay64 [base64], defines the payload for the inSsl-key.
 |  |  |  |  | -pass [pass], defines the password of the inSsl-key-file.
 |  |  |  |  | -name [name], defines an internal friendly name for the inSsl-key-file.
 |  | -CAs, following params are applied to the inSsl certificate-authorities-list.
 |  |  | -path [path], adds one CA certificate to the inSsl list.
 |  |  | -pay64 [base64], adds one CA certificate to the inSsl list.
 | -mask, following params are applied to inMasking context.
 |  | -seed [0-255], inMask seed value.
 | -redir, following params are applied to redirection of port's conns.
 |  | -server [server], defines the destination server.
 |  | -port [number], defines the destination port.
 |  | -layer [mask|ssl|base64], adds a layer to the outStack.
 |  | -ssl, following params are applied to outSsl context.
 |  |  | -cert, following params are applied to outSsl-certificate.
 |  |  |  | -source, following params are applied to outSsl-certificate-source.
 |  |  |  |  | -path [path], defines the path for the certificate to use.
 |  |  |  |  | -key, following params are applied to outSsl-certificate-key.
 |  |  |  |  |  | -path [path], defines the path of the outSsl-key-file.
 |  |  |  |  |  | -pass [pass], defines the password of the outSsl-key-file.
 |  |  |  |  |  | -name [name], defines an internal friendly name for the outSsl-key-file.
 |  |  | -CAs, following params are applied to the outSsl certificate-authorities-list.
 |  |  |  | -path [path], adds one CA certificate to the outSsl list.
 |  |  |  | -pay64 [base64], adds one CA certificate to the outSsl list.
 |  | -mask, following params are applied to outMasking context.
 |  |  | -seed, outMask seed value.
-io, enables stdin/stdout processing, following params are applied to this io.
 | -layer [mask|ssl|base64], adds a layer to the stdin.
 | -ssl, following params are applied to stdin context.
 |  | -cert, following params are applied to stdin-certificate.
 |  |  | -source, following params are applied to stdin-certificate-source.
 |  |  |  | -path [path], defines the path for the certificate to use.
 |  |  |  | -pay64 [base64], defines the payload for the certificate to use.
 |  |  |  | -key, following params are applied to stdin-key.
 |  |  |  |  | -path [path], defines the path of the stdin-key-file.
 |  |  |  |  | -pay64 [base64], defines the payload for the stdin-key.
 |  |  |  |  | -pass [pass], defines the password of the stdin-key-file.
 |  |  |  |  | -name [name], defines an internal friendly name for the stdin-key-file.
 | -mask, following params are applied to stdinMasking context.
 |  | -seed [0-255], stdinMask seed value.
 | -redir, following params are applied to redirection to stdout.
 |  | -layer [mask|ssl|base64], adds a layer to the stdout.
 |  | -ssl, following params are applied to stdout context.
 |  |  | -cert, following params are applied to stdout-certificate.
 |  |  |  | -source, following params are applied to stdout-certificate-source.
 |  |  |  |  | -path [path], defines the path for the certificate to use.
 |  |  |  |  | -key, following params are applied to stdout-certificate-key.
 |  |  |  |  |  | -path [path], defines the path of the stdout-key-file.
 |  |  |  |  |  | -pass [pass], defines the password of the stdout-key-file.
 |  |  |  |  |  | -name [name], defines an internal friendly name for the stdout-key-file.
 |  | -mask, following params are applied to stdoutMasking context.
 |  |  | -seed, stdoutMask seed value.
-cfgEnd, following params are parsed as non-cfg params.
```

Examples:

This command opens a port 8089 for masking and redirecting to port 8090:
```
tunnel-server -cfgStart -port 8089 -redir -server localhost -port 8090 -layer mask -mask -seed 99 -cfgEnd
```

This command opens a port 8090 for unmasking and redirecting to port google:443:
```
tunnel-server -maxConnsAndExit 1 -cfgStart -port 8090 -layer mask -mask -seed 99 -redir -server www.google.com.ni -port 443
```

This command opens a port 8089 for ssl-encryption, masking, and redirecting to port 8090:
```
tunnel-server -cfgStart -CAs -path file.cert -path "file 2.cert" -port 8089 -redir -server "remote.com" -port 8090 -layer ssl -layer mask -ssl -cert -source -path "file 3.cert" -key -path "file 4.key" -CAs -path "file 5.cert" -cfgEnd
```

This command reads stdin and sends the masked data to stdout in base64:
```
tunnel-server -cfgStart -io -redir -layer mask -layer base64 -mask -seed 199 -cfgEnd
```

This command reads stdin in base64 and sends the unmasked data to stdout:
```
tunnel-server -cfgStart -io -layer mask -layer base64 -mask -seed 199 -redir -cfgEnd
```

# Contact

Visit [mortegam.com](https://mortegam.com/) to see other projects.

May you be surrounded by passionate and curious people. :-)



