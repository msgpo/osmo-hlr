#!/usr/bin/env python3
"""
SPDX-License-Identifier: MIT
Copyright 2019 sysmocom s.f.m.c GmbH <info@sysmocom.de>

WARNING: this is just a proof-of-concept implementation, it blocks for every
received SMPP request and is not suitable for servicing more than one request
at a time.

Based on esme.py from RCCN (license changed with permission from author):
https://github.com/Rhizomatica/rccn/blob/master/rccn/esme.py
Copyright 2017 keith <keith@rhizomatica.org>

Forward SMS to the receiver's SMSC, as determined with mslookup.
Requires smpplip (pip3 install --user smpplib) and osmo-mslookup-client.

Example SMPP configuration for osmo-msc.cfg:
smpp
 local-tcp-ip 127.0.0.1 2775
 policy closed
 smpp-first
# outgoing to esme_dgsm.py
 esme OSMPP
  no alert-notifications
  password foo
  default-route
# incoming from esme_dgsm.py
 esme ISMPP
  no alert-notifications
  password foo
"""
import argparse
import json
import logging
import smpplib
import subprocess
import time


def can_handle_pdu(pdu):
    if not isinstance(pdu, smpplib.command.DeliverSM):
        logging.info('PDU is not a DeliverSM, ignoring')
        return False

    if int(pdu.dest_addr_ton) == smpplib.consts.SMPP_TON_INTL:
        logging.info("Unable to handle SMS for %s: SMPP_TON_INTL" %
                     (pdu.destination_addr))
        return False

    return True


def query_mslookup(service_type, id, id_type='msisdn'):
    query_str = '%s.%s.%s' % (service_type, id, id_type)
    logging.info('mslookup: ' + query_str)

    result_line = subprocess.check_output(['osmo-mslookup-client', query_str,
                                           '-f', 'json'])
    if isinstance(result_line, bytes):
        result_line = result_line.decode('ascii')

    logging.info('mslookup result: ' + result_line.rstrip())
    return json.loads(result_line)


def tx_sms(dst_host, dst_port, source, destination, registered_delivery,
           unicode_text):
    smpp_client = smpplib.client.Client(dst_host, dst_port, 90)
    smpp_client.connect()
    smpp_client.bind_transceiver(system_id=args.dst_id, password=args.dst_pass)
    logging.info('Connected to destination SMSC (%s@%s:%s)' % (args.dst_id,
                 dst_host, dst_port))

    pdu = smpp_client.send_message(
        source_addr_ton=smpplib.consts.SMPP_TON_ALNUM,
        source_addr_npi=smpplib.consts.SMPP_NPI_UNK,
        source_addr=source.decode(),
        dest_addr_ton=smpplib.consts.SMPP_TON_SBSCR,
        dest_addr_npi=smpplib.consts.SMPP_NPI_ISDN,
        destination_addr=destination.decode(),
        short_message=unicode_text,
        registered_delivery=registered_delivery,
    )

    smpp_client.unbind()
    smpp_client.disconnect()
    del pdu
    del smpp_client


def rx_deliver_sm(pdu):
    if not can_handle_pdu(pdu):
        return smpplib.consts.SMPP_ESME_RSYSERR

    msisdn = pdu.destination_addr.decode()
    logging.info("Incoming SMS for: " + msisdn)

    if args.sleep:
        logging.info("Sleeping for %i seconds" % (args.sleep))
        time.sleep(args.sleep)
        logging.info("Sleep done")

    if args.always_fail is not None:
        return args.always_fail

    result = query_mslookup("smpp.sms", msisdn)
    if 'v4' not in result or not result['v4']:
        logging.info('No IPv4 result from mslookup! This example only'
                     ' makes use of IPv4, dropping.')
        return smpplib.consts.SMPP_ESME_RSYSERR

    dst_host, dst_port = result['v4']
    tx_sms(dst_host, dst_port, pdu.source_addr,
           pdu.destination_addr, int(pdu.registered_delivery),
           pdu.short_message)

    return smpplib.consts.SMPP_ESME_ROK


def smpp_bind():
    client = smpplib.client.Client(args.src_host, args.src_port, 90)
    client.set_message_received_handler(rx_deliver_sm)
    client.connect()
    client.bind_transceiver(system_id=args.src_id, password=args.src_pass)
    logging.info('Connected to source SMSC (%s@%s:%s)' % (args.src_id,
                 args.src_host, args.src_port))
    logging.info('Waiting for SMS...')
    client.listen()


def main():
    global args
    parser = argparse.ArgumentParser()
    parser.add_argument('--src-host', default='127.0.0.1',
                        help='source SMSC (OsmoMSC) host (default: 127.0.0.1)')
    parser.add_argument('--src-port', default=2775, type=int,
                        help='source SMSC (OsmoMSC) port (default: 2775)')
    parser.add_argument('--src-id', default='OSMPP',
                        help='source system id, as configured in osmo-msc.cfg'
                             ' (default: OSMPP)')
    parser.add_argument('--src-pass', default='foo',
                        help='source system password, as configured in'
                             ' osmo-msc.cfg (default: foo)')
    parser.add_argument('--dst-id', default='ISMPP',
                        help='destination system id, as configured in'
                             ' osmo-msc.cfg (default: ISMPP)')
    parser.add_argument('--dst-pass', default='foo',
                        help='destination system password, as configured in'
                             ' osmo-msc.cfg (default: foo)')
    parser.add_argument('--sleep', default=0, type=float,
                        help='sleep time in seconds before forwarding an SMS,'
                             ' to test multithreading (default: 0)')
    parser.add_argument('--always-fail', default=None, metavar='SMPP_ESME_ERRCODE',
                        help='test delivery failure: always return an error code on Deliver-SM,'
                        ' pass an smpplib error code name like RDELIVERYFAILURE (see smpplib/consts.py),'
                        ' or an SMPP error code in hex digits')
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format='[%(asctime)s]'
                        ' (%(threadName)s) %(message)s', datefmt="%H:%M:%S")

    if args.always_fail:
        resolved = None
        name = 'SMPP_ESME_' + args.always_fail
        if hasattr(smpplib.consts, name):
            resolved = getattr(smpplib.consts, name)
        if resolved is None:
            try:
                resolved = int(args.always_fail, 16)
            except ValueError:
                resolved = None
        if resolved is None:
            print('Invalid argument for --always-fail: %r' % args.always_fail)
            exit(1)
        args.always_fail = resolved
        logging.info('--always-fail: returning error code %s to all Deliver-SM' % hex(args.always_fail))

    smpp_bind()


if __name__ == "__main__":
    main()

# vim: expandtab tabstop=4 shiftwidth=4
