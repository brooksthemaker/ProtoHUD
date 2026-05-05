#!/usr/bin/perl
use strict;
use warnings;
use IO::Socket::INET;

my $port = $ENV{PORT} // 7823;
my $root = ".";

$SIG{CHLD} = 'IGNORE';

my $server = IO::Socket::INET->new(
    LocalAddr => '0.0.0.0',
    LocalPort => $port,
    Proto     => 'tcp',
    Type      => SOCK_STREAM,
    ReuseAddr => 1,
    Listen    => 20,
) or die "Cannot bind port $port: $!\n";

print "Serving on http://localhost:$port/\n";
$| = 1;

while (1) {
    my $client = $server->accept() or next;
    $client->autoflush(1);

    # Read the request line + headers (until blank line)
    my $req_line = <$client>;
    last unless defined $req_line;

    # Drain the rest of the headers
    while (my $hdr = <$client>) {
        last if $hdr =~ /^\r?\n$/;
    }

    chomp $req_line;
    $req_line =~ s/\r//;

    my ($method, $path) = split /\s+/, $req_line;
    $path //= "/";
    $path = "/index.html" if !defined($path) || $path eq "/";
    $path =~ s/\?.*//;    # strip query
    $path =~ s|\.\.||g;   # no dir traversal
    $path =~ s|^/||;      # strip leading slash

    my $file = "$root/$path";

    if (-f $file) {
        open(my $fh, "<:raw", $file) or do {
            print $client "HTTP/1.1 500 Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\nError";
            close $client;
            next;
        };
        local $/;
        my $body = <$fh>;
        close $fh;

        my $len = length($body);
        my $ct  = "text/html; charset=utf-8";
        $ct = "text/css"               if $file =~ /\.css$/i;
        $ct = "application/javascript" if $file =~ /\.js$/i;
        $ct = "image/png"              if $file =~ /\.png$/i;

        print $client
            "HTTP/1.1 200 OK\r\n",
            "Content-Type: $ct\r\n",
            "Content-Length: $len\r\n",
            "Connection: close\r\n",
            "\r\n",
            $body;
    } else {
        my $body = "Not found: $path";
        my $len  = length($body);
        print $client
            "HTTP/1.1 404 Not Found\r\n",
            "Content-Type: text/plain\r\n",
            "Content-Length: $len\r\n",
            "Connection: close\r\n",
            "\r\n",
            $body;
    }

    close $client;
}
