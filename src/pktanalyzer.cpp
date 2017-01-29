/*
*
*    Copyright (C) Max <max1976@mail.ru>
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include <stdint.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <linux/netfilter.h>
#include <memory>

#include "main.h"
#include "AhoCorasickPlus.h"

#include <Poco/Stopwatch.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/URI.h>

#include "sendertask.h"
#include "ndpiwrapper.h"
#include "pktanalyzer.h"

#define iphdr(x)	((struct iphdr *)(x))
#define tcphdr(x)	((struct tcphdr *)(x))

#define __USE_POCO_URI_DECODE

PktAnalyzer::PktAnalyzer(const std::string& name, Poco::NotificationQueue& queue, struct nfqConfig& cfg, nfqThread *parent):
	_name(name),
	_queue(queue),
	_logger(Poco::Logger::get(name)),
	_config(cfg),
	_parent(parent),
	_idleTime(200),
	_stopped(false)
{

}


char PktAnalyzer::from_hex(char ch)
{
	return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

std::string PktAnalyzer::url_decode(std::string text)
{
	char h;
	std::string escaped;
	escaped.reserve(text.length());
	for (auto i = text.begin(), n = text.end(); i != n; ++i)
	{
		std::string::value_type c = (*i);
		if (c == '%')
		{
			if (i[1] && i[2])
			{
				h = from_hex(i[1]) << 4 | from_hex(i[2]);
				if((h >= '0' && h <= '9') || ( h >= 'a' && h <= 'z') || ( h >= 'A' && h <= 'Z'))
				{
					escaped += h;
					i += 2;
				} else {
					escaped += c;
				}
			}
		} else {
			escaped += c;
		}
	}
	return escaped;
}

void PktAnalyzer::dump_file(unsigned char *buf,uint32_t rv, int pkt_id)
{
	std::string s=std::to_string(pkt_id);
	Poco::FileOutputStream pdump("/tmp/packet_dump"+s,std::ios::binary);
	for(uint32_t i=0; i < rv; i++)
	{
		pdump << (unsigned char) buf[i];
	}
	pdump.close();

}

void PktAnalyzer::analyzer(Packet &pkt)
{
	Poco::Stopwatch sw;
	_logger.debug("Got packet from queue");

	unsigned char *full_packet=pkt.get_payload();
	int id = pkt.get_id();
	uint32_t size = pkt.get_size();
	struct nfq_q_handle *qh=pkt.get_qh();
	struct ip *iph = (struct ip *)full_packet;
	struct ip6_hdr *iph6 = (struct ip6_hdr *)full_packet;

	// определяем версию протокола
	int ip_version=0;
	if(iph->ip_v == 6)
		ip_version = 6;
	else if (iph->ip_v == 4)
		ip_version = 4;
	if(!ip_version)
	{
		_logger.error("Unsupported IP protocol version %d for packet id %d",(int) iph->ip_v, id);
		nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
		dump_file(full_packet,size,id);
		return ;
	}

	unsigned char *pkt_data_ptr = NULL;
	struct tcphdr* tcph;

	pkt_data_ptr = full_packet + (ip_version == 4 ? sizeof(struct ip) : sizeof(struct ip6_hdr));

	tcph = (struct tcphdr *) pkt_data_ptr;

	// длина ip заголовка
	int iphlen = iphdr(full_packet)->ihl*4; // ipv4
	if(ip_version == 6)
		iphlen = sizeof(struct ip6_hdr);

	// длина tcp заголовка
	int tcphlen = tcphdr(full_packet+iphlen)->doff*4;

	// общая длина всех заголовков
	uint32_t hlen = iphlen + tcphlen;

	_parent->inc_total_bytes_packets(size);

	// пропускаем пакет без данных
	if(hlen == size)
	{
		nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
		return ;
	}

	int tcp_src_port=ntohs(tcph->source);
	int tcp_dst_port=ntohs(tcph->dest);
	std::unique_ptr<Poco::Net::IPAddress> src_ip;
	std::unique_ptr<Poco::Net::IPAddress> dst_ip;
	if(ip_version == 4)
	{
		src_ip.reset(new Poco::Net::IPAddress(&iph->ip_src,sizeof(in_addr)));
		dst_ip.reset(new Poco::Net::IPAddress(&iph->ip_dst,sizeof(in_addr)));
	} else {
		src_ip.reset(new Poco::Net::IPAddress(&iph6->ip6_src,sizeof(in6_addr)));
		dst_ip.reset(new Poco::Net::IPAddress(&iph6->ip6_dst,sizeof(in6_addr)));
	}

	uint8_t ip_protocol=(ip_version == 4 ? iph->ip_p : iph6->ip6_ctlun.ip6_un1.ip6_un1_nxt);


	{
		Poco::ScopedReadRWLock lock(nfqFilter::_ipportMapMutex);
		IPPortMap::iterator it_ip=nfqFilter::_ipportMap->find(*dst_ip.get());
		if(it_ip != nfqFilter::_ipportMap->end())
		{
			unsigned short port=tcp_dst_port;
			if (it_ip->second.size() == 0 || it_ip->second.find(port) != it_ip->second.end())
			{
				_parent->inc_matched_ip_port();
				if(_config.send_rst)
				{
					_logger.debug("HostList: Send RST to the client (%s) and server (%s) (packet no %d)",src_ip->toString(),dst_ip->toString(),id);
					std::string empty_str;
					SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq,/* flag psh */ (tcph->psh ? 1 : 0 ),empty_str,true));
					_parent->inc_sended_rst();
					nfq_set_verdict(qh,id,NF_DROP,0,NULL);
				} else {
					_logger.debug("HostList: Set mark %d to packet no %d  port %hu",_config.mark_value,id,port);
					_parent->inc_marked_hosts();
					nfq_set_verdict2(qh,id,NF_ACCEPT,_config.mark_value,0,NULL);
				}
				return ;
			}
		}
	}


	// nDPI usage
	sw.reset();
	sw.start();
	nDPIWrapper nw;

	struct ndpi_flow_struct *flow=nw.get_flow();

	uint32_t current_tickt = 0;
	ndpi_protocol protocol = ndpi_detection_process_packet(nfqFilter::my_ndpi_struct, flow, full_packet, size, current_tickt, nw.get_src(), nw.get_dst());

	if(protocol.protocol == NDPI_PROTOCOL_UNKNOWN)
	{
		_logger.debug("Guessing protocol...");
		protocol = ndpi_guess_undetected_protocol(nfqFilter::my_ndpi_struct,
		   ip_protocol,
		   0,//ip
		   tcp_src_port, // sport
		   0,
		   tcp_dst_port); // dport
	}
	_logger.debug("Protocol is %hu/%hu ",protocol.master_protocol,protocol.protocol);
	sw.stop();
	_logger.debug("nDPI protocol detection occupied %ld us",sw.elapsed());
	if(protocol.master_protocol == NDPI_PROTOCOL_SSL || protocol.protocol == NDPI_PROTOCOL_SSL || protocol.protocol == NDPI_PROTOCOL_TOR)
	{
		if(flow->l4.tcp.ssl_seen_client_cert == 1)
		{
			std::string ssl_client;
			_logger.debug("Analysing SSL protocol");
			if(flow->protos.ssl.client_certificate[0] != '\0')
			{
				ssl_client=flow->protos.ssl.client_certificate;
				_logger.debug("SSL client is: %s",ssl_client);
			}
			if(!ssl_client.empty())
			{
				sw.reset();
				sw.start();
				if(_config.lower_host)
					std::transform(ssl_client.begin(), ssl_client.end(), ssl_client.begin(), ::tolower);
				AhoCorasickPlus::Match match;
				std::size_t host_len=ssl_client.length();
				bool found=false;
				{
					Poco::Mutex::ScopedLock lock(nfqFilter::_sslMutex);
					nfqFilter::atm_ssl->search(ssl_client,false);
					while(nfqFilter::atm_ssl->findNext(match) && !found)
					{
						if(match.pattern.length != host_len)
						{
							DomainsMatchType::Iterator it=nfqFilter::_SSLdomainsMatchType->find(match.id);
							bool exact_match=false;
							if(it != nfqFilter::_SSLdomainsMatchType->end())
								exact_match = it->second;
							if(exact_match)
								continue;
							if(ssl_client[host_len-match.pattern.length-1] != '.')
								continue;
						}
						found=true;
					}
				}
				sw.stop();
				_logger.debug("SSL Host seek occupied %ld us, host: %s",sw.elapsed(),ssl_client);
				if(found)
				{
					_parent->inc_matched_ssl();
					if(_config.send_rst)
					{
						_logger.debug("SSLHostList: Send RST to the client (%s) and server (%s) (packet no %d)",src_ip->toString(),dst_ip->toString(),id);
						std::string empty_str;
						SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq,/* flag psh */ (tcph->psh ? 1 : 0 ),empty_str,true));
						_parent->inc_sended_rst();
						nfq_set_verdict(qh,id,NF_DROP,0,NULL);
					} else {
						_logger.debug("SSLHostList: Set mark %d to packet no %d, ssl host name: %s",_config.mark_value,id,ssl_client);
						_parent->inc_marked_ssl();
						nfq_set_verdict2(qh,id,NF_ACCEPT,_config.mark_value,0,NULL);
					}
					return ;
				} else {
					nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
					return ;
				}
			} else {
				if(_config.block_undetected_ssl)
				{
					Poco::ScopedReadRWLock lock(nfqFilter::_sslIpsSetMutex);
					if(nfqFilter::_sslIps->try_search_exact_ip(*dst_ip.get()))
					{
						_parent->inc_matched_ssl_ip();
						_logger.debug("Blocking/Marking SSL client hello packet from %s:%d to %s:%d", src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
						if(_config.send_rst)
						{
							_logger.debug("SSLClientHello: Send RST to the client (%s) and server (%s) (packet no %d)",src_ip->toString(),dst_ip->toString(),id);
							std::string empty_str;
							SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(), dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq,/* flag psh */ (tcph->psh ? 1 : 0 ),empty_str,true));
							_parent->inc_sended_rst();
							nfq_set_verdict(qh,id,NF_DROP,0,NULL);
						} else {
							_logger.debug("SSLClientHello: Set mark %d to packet no %d",_config.mark_value,id);
							_parent->inc_marked_ssl();
							nfq_set_verdict2(qh,id,NF_ACCEPT,_config.mark_value,0,NULL);
						}
						return ;
					}
				}
				_logger.debug("No ssl client certificate found! Accept packet from %s:%d to %s:%d.",src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
			}
		}
		nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
		return ;
	}
	if(protocol.master_protocol != NDPI_PROTOCOL_HTTP && protocol.protocol != NDPI_PROTOCOL_HTTP && protocol.protocol != NDPI_PROTOCOL_DIRECT_DOWNLOAD_LINK)
	{
		_logger.debug("Not http protocol. Protocol is %hu/%hu from %s:%d to %s:%d",protocol.master_protocol,protocol.protocol,src_ip->toString(),tcp_src_port,dst_ip->toString(),tcp_dst_port);
		nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
		return ;
	}

	_logger.debug("Got HTTP protocol");

	std::string host((char *)&flow->host_server_name[0]);
	std::string uri_o(flow->http.url ? flow->http.url : "");
	std::string add_param;
	std::string uri_encoded;
	std::string uri_reserved=";,/?:@&=+$";
	if((flow->http.method == HTTP_METHOD_GET || flow->http.method == HTTP_METHOD_POST || flow->http.method == HTTP_METHOD_HEAD) && !host.empty())
	{
		int dot_del=0;
		if(host[host.length()-1] == '.')
		{
			dot_del=host.length()-1;
			host.erase(dot_del,1);
		}
		if(_config.lower_host)
			std::transform(host.begin(), host.end(), host.begin(), ::tolower);
		sw.reset();
		sw.start();

		AhoCorasickPlus::Match match;
		bool found=false;
		{
			Poco::Mutex::ScopedLock lock(nfqFilter::_domainMapMutex);
			nfqFilter::atm_domains->search(host,false);
			std::size_t host_len=host.length();
			while(nfqFilter::atm_domains->findNext(match) && !found)
			{
				if(match.pattern.length != host_len)
				{
					DomainsMatchType::Iterator it=nfqFilter::_domainsMatchType->find(match.id);
					bool exact_match=false;
					if(it != nfqFilter::_domainsMatchType->end())
						exact_match = it->second;
					if(exact_match)
						continue;
					if(host[host_len-match.pattern.length-1] != '.')
						continue;
				}
				found=true;
			}
		}
		sw.stop();
		_logger.debug("Host seek occupied %ld us",sw.elapsed());
		if(found)
		{
			_logger.debug("Host %s present in domain (file line %u) list from ip %s", host, match.id, src_ip->toString());
			switch (_config.add_p_type)
			{
				case A_TYPE_ID: add_param="id="+std::to_string(match.id);
						break;
				case A_TYPE_URL:
				    Poco::URI::encode(uri_o, uri_reserved, uri_encoded);
				    add_param="url="+uri_encoded;
				    _logger.debug("Params for redirect: '%s'", add_param);
				    break;
				default: break;
			}
			SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port, src_ip.get(), dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq,/* flag psh */ (tcph->psh ? 1 : 0 ),add_param));
			_parent->inc_redirected_domains();
			nfq_set_verdict(qh,id,NF_DROP,0,NULL);
			return ;
		}
		sw.reset();
		sw.start();
		found=false;
		if(flow->http.url)
		{
			std::string uri;
			if(dot_del)
				uri_o.erase(dot_del+7,1);
			try
			{
				Poco::URI uri_p(uri_o);
				uri_p.normalize();
				uri.assign(uri_p.toString());
				if(_config.url_decode)
				{
#ifdef __USE_POCO_URI_DECODE
					Poco::URI::decode(uri_p.toString(),uri);
#else
					uri=url_decode(uri);
#endif
				}
			} catch (Poco::SyntaxException &ex)
			{
				_logger.debug("An SyntaxException occured: '%s' on URI: '%s'",ex.displayText(), uri_o);
				uri.assign(flow->http.url);
			}

			{
				Poco::Mutex::ScopedLock lock(nfqFilter::_urlMapMutex);
				nfqFilter::atm->search(uri,false);
				while(nfqFilter::atm->findNext(match) && !found)
				{
					if(_config.match_url_exactly && uri.length() != match.pattern.length)
						continue;
					found=true;
				}
			}
			sw.stop();
			_logger.debug("URL seek occupied %ld us for uri %s",sw.elapsed(),uri);
			if(found)
			{
				_logger.debug("URL %s present in url (file pos %u) list from ip %s",uri,match.id,src_ip->toString());
				switch (_config.add_p_type)
				{
					case A_TYPE_ID: add_param="id="+std::to_string(match.id);
					    break;
					case A_TYPE_URL: 
					    Poco::URI::encode(uri, uri_reserved, uri_encoded);
					    add_param="url="+uri_encoded;
					    _logger.debug("Params for redirect: '%s'", uri_encoded);
					    break;
					default: break;
				}
				SenderTask::queue.enqueueNotification(new RedirectNotification(tcp_src_port, tcp_dst_port,src_ip.get(),dst_ip.get(),/*acknum*/ tcph->ack_seq, /*seqnum*/ tcph->seq,/* flag psh */ (tcph->psh ? 1 : 0 ),add_param));
				_parent->inc_redirected_urls();
				nfq_set_verdict(qh,id,NF_DROP,0,NULL);
				return ;
			}
		}
	}
	nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
}

void PktAnalyzer::run()
{
	_logger.debug("Starting thread...");
	for (;;)
	{
		Poco::Notification::Ptr pNf(_queue.waitDequeueNotification(_idleTime));
		if (pNf)
		{
			PktNotification::Ptr pPktNf = pNf.cast<PktNotification>();
			if (pPktNf)
			{
				analyzer(pPktNf->pkt());
			}
		}
		Poco::FastMutex::ScopedLock lock(_mutex);
		if(_stopped)
		{
			break;
		}
	}
	_logger.debug("Stopping thread...");
}

void PktAnalyzer::stop()
{
	_stopped=true;
}

