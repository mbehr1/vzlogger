/*
 * Author: Matthias Behr, mbehr (a) mcbehr dot de
 * (c) 2018
 * */

#include "mqtt.hpp"
#include "common.h"
#include "mosquitto.h"
#include <cassert>
#include <sstream>
#include <unistd.h>

// global var:
MqttClient *mqttClient = 0;
volatile bool endMqttClientThread = false;

// class impl.
MqttClient::MqttClient(struct json_object *option) : _enabled(false)
{
	print(log_finest, "MqttClient::MqttClient called", "mqtt");
	if (option)
	{
		assert(json_object_get_type(option) == json_type_object);
		json_object_object_foreach(option, key, local_value)
		{
			enum json_type local_type = json_object_get_type(local_value);

			if (strcmp(key, "enabled") == 0 && local_type == json_type_boolean)
			{
				_enabled = json_object_get_boolean(local_value);
			}
			else if (strcmp(key, "retain") == 0 && local_type == json_type_boolean)
			{
				_retain = json_object_get_boolean(local_value);
			}
			else if (strcmp(key, "rawAndAgg") == 0 && local_type == json_type_boolean)
			{
				_rawAndAgg = json_object_get_boolean(local_value);
			}
			else if (strcmp(key, "port") == 0 && local_type == json_type_int)
			{
				_port = json_object_get_int(local_value);
			}
			else if (strcmp(key, "keepalive") == 0 && local_type == json_type_int)
			{
				_keepalive = json_object_get_int(local_value);
			}
			else if (strcmp(key, "host") == 0 && local_type == json_type_string)
			{
				_host = json_object_get_string(local_value);
			}
			else if (strcmp(key, "user") == 0 && local_type == json_type_string)
			{
				_user = json_object_get_string(local_value);
			}
			else if (strcmp(key, "pass") == 0 && local_type == json_type_string)
			{
				_pwd = json_object_get_string(local_value);
			}
			else if (strcmp(key, "topic") == 0 && local_type == json_type_string)
			{
				_topic = json_object_get_string(local_value);
				// todo check for no $ at start and no / at end
			}
			else
			{
				print(log_alert, "Ignoring invalid field or type: %s=%s", NULL, key,
					  json_object_get_string(local_value));
			}
		}
	}
	else
	{
		throw vz::VZException("config: mqtt no options!");
	}

	// default topic to vzlogger
	if (!_topic.length())
		_topic = std::string("vzlogger/");
	else
		_topic += '/';

	// mosquitto lib init:
	if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
	{
		print(log_alert, "libmosquitto init failed! Stopped.", "mqtt");
		_enabled = false;
	}

	// check mosquitto version:
	int major = -1;
	int minor = -1;
	int revision = -1;
	mosquitto_lib_version(&major, &minor, &revision); // tested with 1.4.15
	print(log_finest, "using libmosquitto %d.%d.%d", "mqtt", major, minor,
		  revision);
	if (major != LIBMOSQUITTO_MAJOR)
	{
		print(log_alert,
			  "Wrong libmosquitto major version! %d vs. expected %d! Stopped.",
			  "mqtt", major, LIBMOSQUITTO_MAJOR);
		_enabled = false;
	}

	if (isConfigured())
	{
		// create an id. for now just use the pid.
		std::ostringstream id;
		id << "vzlogger_" << getpid(); // todo does this need to be in sync with topic?
		_mcs = mosquitto_new(id.str().c_str(), true, this);
		if (!_mcs)
		{
			print(log_alert, "mosquitto_new failed! Stopped!", "mqtt");
			_enabled = false;
		}
		else
		{
			// tell mosquitto that it should be thread safe:
			int res = mosquitto_threaded_set(_mcs, true);
			if (res != MOSQ_ERR_SUCCESS)
			{
				print(log_warning, "mosquitto_threaded_set returned %d!", "mqtt", res);
			}
			// set username&pwd
			if ((_user.length() or _pwd.length()) and mosquitto_username_pw_set(_mcs, _user.c_str(), _pwd.c_str()) !=
														  MOSQ_ERR_SUCCESS)
			{
				print(log_warning,
					  "mosquitto_username_pw_set failed! Continuing anyhow.", "mqtt");
			}

			mosquitto_connect_callback_set(_mcs, [](struct mosquitto *mosq, void *obj, int res) {
				static_cast<MqttClient *>(obj)->connect_callback(mosq, res);
			});
			mosquitto_disconnect_callback_set(_mcs, [](struct mosquitto *mosq, void *obj, int res) {
				static_cast<MqttClient *>(obj)->disconnect_callback(mosq, res);
			});

			mosquitto_message_callback_set(_mcs, [](struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
				static_cast<MqttClient *>(obj)->message_callback(mosq, msg);
			});

			// now connect. we use sync interface with spe. thread calling mosquitto_loop
			res = mosquitto_connect(_mcs, _host.c_str(), _port, _keepalive);
			if (res != MOSQ_ERR_SUCCESS)
			{
				print(log_alert, "mosquitto_connect failed. res=%d (%s)! Stopped!", "mqtt",
					  res, strerror(errno));
				_enabled = false;
			}
		}
	}
}

bool MqttClient::isConfigured() const
{
	if (!_enabled)
		return false;
	if (!_port)
		print(log_finest, "mqtt port not configured!", "mqtt");
	if (!_host.length())
		print(log_warning, "mqtt host not configured!", "mqtt");

	return _port != 0 and _host.length();
}

MqttClient::~MqttClient()
{
	print(log_finest, "~MqttClient called", "mqtt");

	if (_mcs)
	{
		assert(endMqttClientThread);

		mosquitto_disconnect(_mcs);
		// we call mosquitto_loop at least once here as the thread should be stopped already:
		int res = mosquitto_loop(_mcs, 50, 1);
		if (res != MOSQ_ERR_SUCCESS and res != MOSQ_ERR_NO_CONN)
		{
			print(log_warning, "mosquitto_loop returned %d", "mqtt", res);
		}
		mosquitto_destroy(_mcs);
	}
	mosquitto_lib_cleanup(); // this assumes nobody else is using libmosquitto!
}

void MqttClient::ChannelEntry::generateNames(const std::string &prefix, Channel &ch)
{
	_fullTopicRaw = prefix;
	_fullTopicRaw += ch.name(); // todo this converts from std::string to const char and back...
	// todo use ch.identifier based name here? (e.g. to get obis code?)

	_sendAgg = ch.buffer() && ch.buffer()->get_aggmode() != Buffer::aggmode::NONE;

	_fullTopicAgg = _fullTopicRaw;
	_announceName = _fullTopicRaw;

	_fullTopicRaw += "/raw";
	_fullTopicAgg += "/agg";
	_announceName += "/uuid";
	_announceValue = ch.uuid();
}

void MqttClient::publish(Channel::Ptr ch, Reading &rds, bool aggregate)
{
	// take care: this function must be thread safe and non-blocking!
	// for now we do only call this from read_thread and our mqtt_client thread doesn't harm here
	// mosquitto_publish doesn't seem to be blocking. needs further investigation!

	if (!ch)
		return;
	if (!_mcs)
		return;

	// search for cached values:
	auto it = _chMap.find(ch->name());
	if (it == _chMap.end())
	{
		ChannelEntry entry;
		entry.generateNames(_topic, (*ch));
		if (entry._sendAgg && !_rawAndAgg)
			entry._sendRaw = false;

		it = _chMap.emplace(std::make_pair(ch->name(), entry)).first;
	}

	assert(it != _chMap.end());
	ChannelEntry &entry = (*it).second;
	// do we need to announce the uuid?
	if (!entry._announced && entry._announceName.length())
	{
		int res = mosquitto_publish(_mcs, 0,
									entry._announceName.c_str(),
									entry._announceValue.length(),
									entry._announceValue.c_str(), 0, _retain);
		if (res != MOSQ_ERR_SUCCESS)
		{
			print(log_finest, "mosquitto_publish announce returned %d", "mqtt", res);
		}
		else
		{
			entry._announced = true;
		}
	}

	std::string &topic = aggregate ? entry._fullTopicAgg : entry._fullTopicRaw;
	if ((entry._sendAgg and aggregate) or (entry._sendRaw && !aggregate))
	{
		std::string payload = std::to_string(rds.value());
		print(log_finest, "publish %s=%s", "mqtt", topic.c_str(), payload.c_str());

		int res = mosquitto_publish(_mcs, 0,
									topic.c_str(), payload.length(), payload.c_str(), 0, _retain);
		if (res != MOSQ_ERR_SUCCESS)
		{
			print(log_finest, "mosquitto_publish returned %d", "mqtt", res);
		}
	}
}

void MqttClient::connect_callback(struct mosquitto *mosq, int result)
{
	print(log_finest, "connect_callback called, res=%d", "mqtt", result);
	switch (result)
	{
	case MOSQ_ERR_SUCCESS:
		_isConnected = true;
		break;
	default:
		_isConnected = false;
		break;
	}
}

void MqttClient::disconnect_callback(struct mosquitto *mosq, int result)
{
	print(log_finest, "disconnect_callback called, res=%d", "mqtt", result);
	_isConnected = false;
}

void MqttClient::message_callback(struct mosquitto *mosq, const struct mosquitto_message *msg)
{
	print(log_finest, "message_callback called", "mqtt");
}

void *mqtt_client_thread(void *arg)
{
	print(log_debug, "Start mqtt_client_thread", "mqtt");

	if (mqttClient)
	{
		while (!endMqttClientThread)
		{
			int res = mosquitto_loop(mqttClient->_mcs, 1000, 1);
			if (res != MOSQ_ERR_SUCCESS)
			{
				print(log_warning, "mosquitto_loop returned %d. trying reconnect", "mqtt", res);
				sleep(1);
				res = mosquitto_reconnect(mqttClient->_mcs);
				if (res != MOSQ_ERR_SUCCESS)
				{
					print(log_warning, "mosquitto_reconnect returned %d", "mqtt", res);
					// todo investigate: will next loop return same != SUCCESS and trigger recon?
				}
				else
				{
					print(log_finest, "mosquitto_reconnect succeeded", "mqtt");
				}
			}
		}
	}

	print(log_debug, "Stopped mqtt_client_thread", "mqtt");
	return 0;
}

void end_mqtt_client_thread()
{
	endMqttClientThread = true;
}