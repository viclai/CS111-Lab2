#! /usr/bin/perl -w

open(FOO, "osprd.c") || die "Did you delete osprd.c?";
$lines = 0;
$lines++ while defined($_ = <FOO>);
close FOO;

@tests = (
    # Test case 1: The process requesting for a notification reads after 
    #              getting the notification. This is like reading a 
    #              scoreboard for a sporting event each time it updates!
    [ 
      './osprdaccess -n -r 4 & ' .
      'echo foo | ./osprdaccess -w 4' ,
      "foo" 
    ],

    # Test case 2: The process requesting for a notification writes after 
    #              getting the notification.
    [
      'echo foo | ./osprdaccess -n -w 3 & ' .
      './osprdaccess -r 7 -l 5 & ' .
      'echo bobbar | ./osprdaccess -w 7 -l ' , 
      "foobar"
    ],

    # Test case 3: The process requests for a notification of the third 
    #              fourth of the disk. When it gets the notification, it 
    #              reads from the first fourth of the disk. 
    [
      './osprdaccess -r 7 -n 3 & ' .
      'echo gotcha | ./osprdaccess -w 7 && ' .
      'echo writing | ./osprdaccess -o 8192 -w 8' ,
      "gotcha"
    ],

    # Test case 4: The process requests a notification of the last fourth 
    #              of the disk. When it gets the notification, it reads what 
    #              was written. This test involves 2 processes acquiring the 
    #              write lock. 
    [
      './osprdaccess -r 11 -o 12288 -n 4 /dev/osprdb & ' .
      'echo "UCLA loses!" | ./osprdaccess -w 12 -l -o 12276 /dev/osprdb &' .
      'echo "UCLA wins!" | ./osprdaccess -w 11 -o 12288 -d 5 -l /dev/osprdb' ,
      "UCLA wins!"
    ],

    # Test case 5: The process requests a notification for the second fourth 
    #              of the disk. When it gets the notification, it reads what 
    #              was written. This test involves 2 processes trying to 
    #              acquire the write lock. 
    [
      './osprdaccess -r 9 -o 4096 -n 2 /dev/osprdc & ' .
      'echo whatever | ./osprdaccess -w 9 -o 4096 -L -d 3 /dev/osprdc &' .
      'sleep 3 && ' .
      'echo sure | ./osprdaccess -w 5 -o 4096 -L /dev/osprdc' ,
      "ioctl OSPRDIOCTRYACQUIRE: Device or resource busy whatever"
    ],

    );

my($ntest) = 0;

my($sh) = "bash";
my($tempfile) = "lab2test.txt";
my($ntestfailed) = 0;
my($ntestdone) = 0;
my($zerodiskcmd) = "./osprdaccess -w -z";
my(@disks) = ("/dev/osprda", "/dev/osprdb", "/dev/osprdc", "/dev/osprdd");

my(@testarr, $anytests);
foreach $arg (@ARGV) {
    if ($arg =~ /^\d+$/) {
	$anytests = 1;
	$testarr[$arg] = 1;
    }
}

foreach $test (@tests) {

    $ntest++;
    next if $anytests && !$testarr[$ntest];

    # clean up the disk for the next test
    foreach $disk (@disks) {
	`$sh <<< "$zerodiskcmd $disk"`
    }

    $ntestdone++;
    print STDOUT "Starting test $ntest\n";
    my($in, $want) = @$test;
    open(F, ">$tempfile") || die;
    print F $in, "\n";
    print STDERR $in, "\n";
    close(F);
    $result = `$sh < $tempfile 2>&1`;
    $result =~ s|\[\d+\]||g;
    $result =~ s|^\s+||g;
    $result =~ s|\s+| |g;
    $result =~ s|\s+$||;

    next if $result eq $want;
    next if $want eq 'Syntax error [NULL]' && $result eq '[NULL]';
    next if $result eq $want;
    print STDERR "Test $ntest FAILED!\n  input was \"$in\"\n  expected output like \"$want\"\n  got \"$result\"\n";
    $ntestfailed++;
}

unlink($tempfile);
my($ntestpassed) = $ntestdone - $ntestfailed;
print "$ntestpassed of $ntestdone tests passed\n";
exit(0);
