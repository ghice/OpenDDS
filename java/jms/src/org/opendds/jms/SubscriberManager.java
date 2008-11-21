/*
 * $Id$
 */

package org.opendds.jms;

import javax.jms.JMSException;
import javax.resource.ResourceException;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

import DDS.DomainParticipant;
import DDS.Subscriber;
import DDS.SubscriberQosHolder;
import OpenDDS.DCPS.transport.AttachStatus;
import OpenDDS.DCPS.transport.TransportImpl;

import org.opendds.jms.common.PartitionHelper;
import org.opendds.jms.qos.QosPolicies;
import org.opendds.jms.qos.SubscriberQosPolicy;
import org.opendds.jms.resource.ConnectionRequestInfoImpl;
import org.opendds.jms.resource.ManagedConnectionImpl;

/**
 * @author  Steven Stallion
 * @version $Revision$
 */
public class SubscriberManager {
    private static Log log = LogFactory.getLog(SubscriberManager.class);

    private ManagedConnectionImpl connection;
    private ConnectionRequestInfoImpl cxRequestInfo;
    private Subscriber remoteSubscriber;
    private Subscriber localSubscriber;

    public SubscriberManager(ManagedConnectionImpl connection) throws ResourceException {
        this.connection = connection;
        cxRequestInfo = connection.getConnectionRequestInfo();
    }

    protected Subscriber createSubscriber(boolean noLocal) throws JMSException {
        SubscriberQosHolder holder =
            new SubscriberQosHolder(QosPolicies.newSubscriberQos());

        DomainParticipant participant = connection.getParticipant();
        participant.get_default_subscriber_qos(holder);

        SubscriberQosPolicy policy = cxRequestInfo.getSubscriberQosPolicy();
        policy.setQos(holder.value);

        // Set PARTITION QosPolicy to support the noLocal client
        // specifier on created MessageConsumer instances:
        if (noLocal) {
            holder.value.partition = PartitionHelper.matchAll();
        } else {
            holder.value.partition = PartitionHelper.negate(connection.getConnectionId());
        }

        Subscriber subscriber = participant.create_subscriber(holder.value, null);
        if (subscriber == null) {
            throw new JMSException("Unable to create Subscriber; please check logs");
        }
        if (log.isDebugEnabled()) {
            log.debug(String.format("Created %s using %s", subscriber, holder.value));
        }

        TransportImpl transport = cxRequestInfo.getSubscriberTransport();
        if (transport.attach_to_subscriber(subscriber) != AttachStatus.ATTACH_OK) {
            throw new JMSException("Unable to attach to Transport; please check logs");
        }
        if (log.isDebugEnabled()) {
            log.debug(String.format("Attached %s to %s", subscriber, transport));
        }

        return subscriber;
    }

    public synchronized Subscriber getLocalSubscriber() throws JMSException {
        if (localSubscriber == null) {
            localSubscriber = createSubscriber(false);
        }
        return localSubscriber;
    }

    public synchronized Subscriber getRemoteSubscriber() throws JMSException {
        if (remoteSubscriber == null) {
            remoteSubscriber = createSubscriber(true);
        }
        return remoteSubscriber;
    }
}
