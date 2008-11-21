/*
 * $Id$
 */

package org.opendds.jms.resource;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import javax.resource.NotSupportedException;
import javax.resource.ResourceException;
import javax.resource.spi.ConnectionEvent;
import javax.resource.spi.ConnectionEventListener;
import javax.resource.spi.ConnectionRequestInfo;
import javax.resource.spi.IllegalStateException;
import javax.resource.spi.LocalTransaction;
import javax.resource.spi.ManagedConnection;
import javax.resource.spi.ManagedConnectionMetaData;
import javax.security.auth.Subject;
import javax.transaction.xa.XAResource;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

import DDS.DomainParticipant;
import DDS.DomainParticipantFactory;
import DDS.DomainParticipantQosHolder;
import DDS.RETCODE_OK;
import OpenDDS.DCPS.DomainParticipantExt;
import OpenDDS.DCPS.DomainParticipantExtHelper;
import OpenDDS.DCPS.TheParticipantFactory;
import OpenDDS.JMS.MessagePayloadTypeSupportImpl;

import org.opendds.jms.ConnectionImpl;
import org.opendds.jms.PublisherManager;
import org.opendds.jms.SubscriberManager;
import org.opendds.jms.common.Version;
import org.opendds.jms.common.lang.Objects;
import org.opendds.jms.qos.ParticipantQosPolicy;
import org.opendds.jms.qos.QosPolicies;

/**
 * @author Steven Stallion
 * @version $Revision$
 */
public class ManagedConnectionImpl implements ManagedConnection {
    private static Log log = LogFactory.getLog("DOMAIN|1");

    private boolean destroyed;
    private Subject subject;
    private ConnectionRequestInfoImpl cxRequestInfo;
    private int domainId;
    private DomainParticipant participant;
    private String typeName;
    private PublisherManager publishers;
    private SubscriberManager subscribers;
    private PrintWriter out; // unused

    private List<ConnectionImpl> handles =
        new ArrayList<ConnectionImpl>();

    private List<ConnectionEventListener> listeners =
        new ArrayList<ConnectionEventListener>();

    public ManagedConnectionImpl(Subject subject,
                                 ConnectionRequestInfoImpl cxRequestInfo) throws ResourceException {
        this.subject = subject;
        this.cxRequestInfo = cxRequestInfo;

        domainId = cxRequestInfo.getDomainId();

        DomainParticipantFactory dpf = TheParticipantFactory.getInstance();
        if (dpf == null) {
            throw new ResourceException("Unable to get DomainParticipantFactory instance; please check logs");
        }
        if (log.isDebugEnabled()) {
            log.debug(String.format("[%d] Using %s", domainId, dpf));
        }

        DomainParticipantQosHolder holder =
            new DomainParticipantQosHolder(QosPolicies.newParticipantQos());

        dpf.get_default_participant_qos(holder);

        ParticipantQosPolicy policy = cxRequestInfo.getParticipantQosPolicy();
        policy.setQos(holder.value);

        participant = dpf.create_participant(cxRequestInfo.getDomainId(), holder.value, null);
        if (participant == null) {
            throw new ResourceException("Unable to create DomainParticipant; please check logs");
        }
        if (log.isDebugEnabled()) {
            log.debug(String.format("[%d] Created %s %s", domainId, participant, holder.value));
        }

        MessagePayloadTypeSupportImpl ts = new MessagePayloadTypeSupportImpl();
        if (ts.register_type(participant, "") != RETCODE_OK.value) {
            throw new ResourceException("Unable to register type; please check logs");
        }
        typeName = ts.get_type_name();

        if (log.isDebugEnabled()) {
            log.debug(String.format("[DOM|%d] Registered %s", domainId, typeName));
        }

        publishers = new PublisherManager(this);
        if (log.isDebugEnabled()) {
            log.debug(String.format("[D|%d] Created %s", domainId, publishers));
        }

        subscribers = new SubscriberManager(this);
        if (log.isDebugEnabled()) {
            log.debug(String.format("[%d] Created %s", domainId, subscribers));
        }

        if (log.isDebugEnabled()) {
            log.debug(String.format("[%d] Connection ID is %s", domainId, getConnectionId()));
        }
    }

    public boolean isDestroyed() {
        return destroyed;
    }

    public Subject getSubject() {
        return subject;
    }

    public ConnectionRequestInfoImpl getConnectionRequestInfo() {
        return cxRequestInfo;
    }

    public String getConnectionId() {
        DomainParticipantExt ext = DomainParticipantExtHelper.narrow(participant);
        return String.format("%08x%08x", ext.get_federation_id(), ext.get_participant_id());
    }

    public DomainParticipant getParticipant() {
        return participant;
    }

    public String getTypeName() {
        return typeName;
    }

    public PublisherManager getPublishers() {
        return publishers;
    }

    public SubscriberManager getSubscribers() {
        return subscribers;
    }

    public PrintWriter getLogWriter() {
        return out;
    }

    public void setLogWriter(PrintWriter out) {
        this.out = out;
    }

    public void addConnectionEventListener(ConnectionEventListener listener) {
        synchronized (listeners) {
            listeners.add(listener);
        }
    }

    public void removeConnectionEventListener(ConnectionEventListener listener) {
        synchronized (listeners) {
            listeners.remove(listener);
        }
    }

    public void associateConnection(Object o) throws ResourceException {
        checkDestroyed();

        if (!(o instanceof ConnectionImpl)) {
            throw new IllegalArgumentException();
        }

        ConnectionImpl handle = (ConnectionImpl) o;
        handle.setManagedConnection(this);

        synchronized (handles) {
            handles.add(handle);
        }
    }

    public Object getConnection(Subject subject,
                                ConnectionRequestInfo cxRequestInfo) throws ResourceException {
        checkDestroyed();

        ConnectionImpl handle = new ConnectionImpl(this);

        synchronized (handles) {
            handles.add(handle);
        }

        return handle; // re-configuration not supported
    }

    public XAResource getXAResource() throws ResourceException {
        throw new NotSupportedException(); // transactions not supported
    }

    public LocalTransaction getLocalTransaction() throws ResourceException {
        throw new NotSupportedException(); // transactions not supported
    }

    public boolean matches(Subject subject, ConnectionRequestInfo cxRequestInfo) {
        return Objects.equals(this.subject, subject)
            && Objects.equals(this.cxRequestInfo, cxRequestInfo);
    }

    public synchronized void cleanup() throws ResourceException {
        checkDestroyed();

        synchronized (handles) {
            for (ConnectionImpl handle : handles) {
                handle.close(false);
            }
            handles.clear();
        }
    }

    public synchronized void destroy() throws ResourceException {
        checkDestroyed();

        cleanup();

        participant.delete_contained_entities();

        subject = null;
        cxRequestInfo = null;
        participant = null;
        typeName = null;
        publishers = null;
        subscribers = null;

        destroyed = true;
    }

    public ManagedConnectionMetaData getMetaData() throws ResourceException {
        return new ManagedConnectionMetaData() {
            private Version version = Version.getInstance();

            public String getEISProductName() {
                return version.getProductName();
            }

            public String getEISProductVersion() {
                return version.getDDSVersion();
            }

            public int getMaxConnections() {
                return 0;
            }

            public String getUserName() {
                return null; // authentication not supported
            }
        };
    }

    protected void checkDestroyed() throws ResourceException {
        if (isDestroyed()) {
            throw new IllegalStateException();
        }
    }

    public void notifyClosed(ConnectionImpl handle) {
        ConnectionEvent event = new ConnectionEvent(this, ConnectionEvent.CONNECTION_CLOSED);
        event.setConnectionHandle(handle);

        notifyListeners(event);
    }

    protected void notifyListeners(ConnectionEvent event) {
        synchronized (listeners) {
            for (ConnectionEventListener listener : listeners) {
                switch (event.getId()) {
                    case ConnectionEvent.CONNECTION_CLOSED:
                        listener.connectionClosed(event);
                        break;

                    case ConnectionEvent.LOCAL_TRANSACTION_STARTED:
                        listener.localTransactionStarted(event);
                        break;

                    case ConnectionEvent.LOCAL_TRANSACTION_COMMITTED:
                        listener.localTransactionCommitted(event);
                        break;

                    case ConnectionEvent.LOCAL_TRANSACTION_ROLLEDBACK:
                        listener.localTransactionRolledback(event);
                        break;

                    case ConnectionEvent.CONNECTION_ERROR_OCCURRED:
                        listener.connectionErrorOccurred(event);
                        break;
                }
            }
        }
    }
}
