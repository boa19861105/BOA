/*
 *  The Serio abstraction module
 *
 *  Copyright (c) 1999-2004 Vojtech Pavlik
 *  Copyright (c) 2004 Dmitry Torokhov
 *  Copyright (c) 2003 Daniele Bellucci
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/stddef.h>
#include <linux/module.h>
#include <linux/serio.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Serio abstraction core");
MODULE_LICENSE("GPL");

static DEFINE_MUTEX(serio_mutex);

static LIST_HEAD(serio_list);

static struct bus_type serio_bus;

static void serio_add_port(struct serio *serio);
static int serio_reconnect_port(struct serio *serio);
static void serio_disconnect_port(struct serio *serio);
static void serio_reconnect_subtree(struct serio *serio);
static void serio_attach_driver(struct serio_driver *drv);

static int serio_connect_driver(struct serio *serio, struct serio_driver *drv)
{
	int retval;

	mutex_lock(&serio->drv_mutex);
	retval = drv->connect(serio, drv);
	mutex_unlock(&serio->drv_mutex);

	return retval;
}

static int serio_reconnect_driver(struct serio *serio)
{
	int retval = -1;

	mutex_lock(&serio->drv_mutex);
	if (serio->drv && serio->drv->reconnect)
		retval = serio->drv->reconnect(serio);
	mutex_unlock(&serio->drv_mutex);

	return retval;
}

static void serio_disconnect_driver(struct serio *serio)
{
	mutex_lock(&serio->drv_mutex);
	if (serio->drv)
		serio->drv->disconnect(serio);
	mutex_unlock(&serio->drv_mutex);
}

static int serio_match_port(const struct serio_device_id *ids, struct serio *serio)
{
	while (ids->type || ids->proto) {
		if ((ids->type == SERIO_ANY || ids->type == serio->id.type) &&
		    (ids->proto == SERIO_ANY || ids->proto == serio->id.proto) &&
		    (ids->extra == SERIO_ANY || ids->extra == serio->id.extra) &&
		    (ids->id == SERIO_ANY || ids->id == serio->id.id))
			return 1;
		ids++;
	}
	return 0;
}


static int serio_bind_driver(struct serio *serio, struct serio_driver *drv)
{
	int error;

	if (serio_match_port(drv->id_table, serio)) {

		serio->dev.driver = &drv->driver;
		if (serio_connect_driver(serio, drv)) {
			serio->dev.driver = NULL;
			return -ENODEV;
		}

		error = device_bind_driver(&serio->dev);
		if (error) {
			dev_warn(&serio->dev,
				 "device_bind_driver() failed for %s (%s) and %s, error: %d\n",
				 serio->phys, serio->name,
				 drv->description, error);
			serio_disconnect_driver(serio);
			serio->dev.driver = NULL;
			return error;
		}
	}
	return 0;
}

static void serio_find_driver(struct serio *serio)
{
	int error;

	error = device_attach(&serio->dev);
	if (error < 0)
		dev_warn(&serio->dev,
			 "device_attach() failed for %s (%s), error: %d\n",
			 serio->phys, serio->name, error);
}



enum serio_event_type {
	SERIO_RESCAN_PORT,
	SERIO_RECONNECT_PORT,
	SERIO_RECONNECT_SUBTREE,
	SERIO_REGISTER_PORT,
	SERIO_ATTACH_DRIVER,
};

struct serio_event {
	enum serio_event_type type;
	void *object;
	struct module *owner;
	struct list_head node;
};

static DEFINE_SPINLOCK(serio_event_lock);	
static LIST_HEAD(serio_event_list);

static struct serio_event *serio_get_event(void)
{
	struct serio_event *event = NULL;
	unsigned long flags;

	spin_lock_irqsave(&serio_event_lock, flags);

	if (!list_empty(&serio_event_list)) {
		event = list_first_entry(&serio_event_list,
					 struct serio_event, node);
		list_del_init(&event->node);
	}

	spin_unlock_irqrestore(&serio_event_lock, flags);
	return event;
}

static void serio_free_event(struct serio_event *event)
{
	module_put(event->owner);
	kfree(event);
}

static void serio_remove_duplicate_events(void *object,
					  enum serio_event_type type)
{
	struct serio_event *e, *next;
	unsigned long flags;

	spin_lock_irqsave(&serio_event_lock, flags);

	list_for_each_entry_safe(e, next, &serio_event_list, node) {
		if (object == e->object) {
			if (type != e->type)
				break;

			list_del_init(&e->node);
			serio_free_event(e);
		}
	}

	spin_unlock_irqrestore(&serio_event_lock, flags);
}

static void serio_handle_event(struct work_struct *work)
{
	struct serio_event *event;

	mutex_lock(&serio_mutex);

	while ((event = serio_get_event())) {

		switch (event->type) {

		case SERIO_REGISTER_PORT:
			serio_add_port(event->object);
			break;

		case SERIO_RECONNECT_PORT:
			serio_reconnect_port(event->object);
			break;

		case SERIO_RESCAN_PORT:
			serio_disconnect_port(event->object);
			serio_find_driver(event->object);
			break;

		case SERIO_RECONNECT_SUBTREE:
			serio_reconnect_subtree(event->object);
			break;

		case SERIO_ATTACH_DRIVER:
			serio_attach_driver(event->object);
			break;
		}

		serio_remove_duplicate_events(event->object, event->type);
		serio_free_event(event);
	}

	mutex_unlock(&serio_mutex);
}

static DECLARE_WORK(serio_event_work, serio_handle_event);

static int serio_queue_event(void *object, struct module *owner,
			     enum serio_event_type event_type)
{
	unsigned long flags;
	struct serio_event *event;
	int retval = 0;

	spin_lock_irqsave(&serio_event_lock, flags);

	list_for_each_entry_reverse(event, &serio_event_list, node) {
		if (event->object == object) {
			if (event->type == event_type)
				goto out;
			break;
		}
	}

	event = kmalloc(sizeof(struct serio_event), GFP_ATOMIC);
	if (!event) {
		pr_err("Not enough memory to queue event %d\n", event_type);
		retval = -ENOMEM;
		goto out;
	}

	if (!try_module_get(owner)) {
		pr_warning("Can't get module reference, dropping event %d\n",
			   event_type);
		kfree(event);
		retval = -EINVAL;
		goto out;
	}

	event->type = event_type;
	event->object = object;
	event->owner = owner;

	list_add_tail(&event->node, &serio_event_list);
	queue_work(system_long_wq, &serio_event_work);

out:
	spin_unlock_irqrestore(&serio_event_lock, flags);
	return retval;
}

static void serio_remove_pending_events(void *object)
{
	struct serio_event *event, *next;
	unsigned long flags;

	spin_lock_irqsave(&serio_event_lock, flags);

	list_for_each_entry_safe(event, next, &serio_event_list, node) {
		if (event->object == object) {
			list_del_init(&event->node);
			serio_free_event(event);
		}
	}

	spin_unlock_irqrestore(&serio_event_lock, flags);
}

static struct serio *serio_get_pending_child(struct serio *parent)
{
	struct serio_event *event;
	struct serio *serio, *child = NULL;
	unsigned long flags;

	spin_lock_irqsave(&serio_event_lock, flags);

	list_for_each_entry(event, &serio_event_list, node) {
		if (event->type == SERIO_REGISTER_PORT) {
			serio = event->object;
			if (serio->parent == parent) {
				child = serio;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&serio_event_lock, flags);
	return child;
}


static ssize_t serio_show_description(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%s\n", serio->name);
}

static ssize_t serio_show_modalias(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);

	return sprintf(buf, "serio:ty%02Xpr%02Xid%02Xex%02X\n",
			serio->id.type, serio->id.proto, serio->id.id, serio->id.extra);
}

static ssize_t serio_show_id_type(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%02x\n", serio->id.type);
}

static ssize_t serio_show_id_proto(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%02x\n", serio->id.proto);
}

static ssize_t serio_show_id_id(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%02x\n", serio->id.id);
}

static ssize_t serio_show_id_extra(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%02x\n", serio->id.extra);
}

static DEVICE_ATTR(type, S_IRUGO, serio_show_id_type, NULL);
static DEVICE_ATTR(proto, S_IRUGO, serio_show_id_proto, NULL);
static DEVICE_ATTR(id, S_IRUGO, serio_show_id_id, NULL);
static DEVICE_ATTR(extra, S_IRUGO, serio_show_id_extra, NULL);

static struct attribute *serio_device_id_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_proto.attr,
	&dev_attr_id.attr,
	&dev_attr_extra.attr,
	NULL
};

static struct attribute_group serio_id_attr_group = {
	.name	= "id",
	.attrs	= serio_device_id_attrs,
};

static const struct attribute_group *serio_device_attr_groups[] = {
	&serio_id_attr_group,
	NULL
};

static ssize_t serio_rebind_driver(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct serio *serio = to_serio_port(dev);
	struct device_driver *drv;
	int error;

	error = mutex_lock_interruptible(&serio_mutex);
	if (error)
		return error;

	if (!strncmp(buf, "none", count)) {
		serio_disconnect_port(serio);
	} else if (!strncmp(buf, "reconnect", count)) {
		serio_reconnect_subtree(serio);
	} else if (!strncmp(buf, "rescan", count)) {
		serio_disconnect_port(serio);
		serio_find_driver(serio);
		serio_remove_duplicate_events(serio, SERIO_RESCAN_PORT);
	} else if ((drv = driver_find(buf, &serio_bus)) != NULL) {
		serio_disconnect_port(serio);
		error = serio_bind_driver(serio, to_serio_driver(drv));
		serio_remove_duplicate_events(serio, SERIO_RESCAN_PORT);
	} else {
		error = -EINVAL;
	}

	mutex_unlock(&serio_mutex);

	return error ? error : count;
}

static ssize_t serio_show_bind_mode(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct serio *serio = to_serio_port(dev);
	return sprintf(buf, "%s\n", serio->manual_bind ? "manual" : "auto");
}

static ssize_t serio_set_bind_mode(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct serio *serio = to_serio_port(dev);
	int retval;

	retval = count;
	if (!strncmp(buf, "manual", count)) {
		serio->manual_bind = true;
	} else if (!strncmp(buf, "auto", count)) {
		serio->manual_bind = false;
	} else {
		retval = -EINVAL;
	}

	return retval;
}

static struct device_attribute serio_device_attrs[] = {
	__ATTR(description, S_IRUGO, serio_show_description, NULL),
	__ATTR(modalias, S_IRUGO, serio_show_modalias, NULL),
	__ATTR(drvctl, S_IWUSR, NULL, serio_rebind_driver),
	__ATTR(bind_mode, S_IWUSR | S_IRUGO, serio_show_bind_mode, serio_set_bind_mode),
	__ATTR_NULL
};


static void serio_release_port(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);

	kfree(serio);
	module_put(THIS_MODULE);
}

static void serio_init_port(struct serio *serio)
{
	static atomic_t serio_no = ATOMIC_INIT(0);

	__module_get(THIS_MODULE);

	INIT_LIST_HEAD(&serio->node);
	INIT_LIST_HEAD(&serio->child_node);
	INIT_LIST_HEAD(&serio->children);
	spin_lock_init(&serio->lock);
	mutex_init(&serio->drv_mutex);
	device_initialize(&serio->dev);
	dev_set_name(&serio->dev, "serio%ld",
			(long)atomic_inc_return(&serio_no) - 1);
	serio->dev.bus = &serio_bus;
	serio->dev.release = serio_release_port;
	serio->dev.groups = serio_device_attr_groups;
	if (serio->parent) {
		serio->dev.parent = &serio->parent->dev;
		serio->depth = serio->parent->depth + 1;
	} else
		serio->depth = 0;
	lockdep_set_subclass(&serio->lock, serio->depth);
}

static void serio_add_port(struct serio *serio)
{
	struct serio *parent = serio->parent;
	int error;

	if (parent) {
		serio_pause_rx(parent);
		list_add_tail(&serio->child_node, &parent->children);
		serio_continue_rx(parent);
	}

	list_add_tail(&serio->node, &serio_list);

	if (serio->start)
		serio->start(serio);

	error = device_add(&serio->dev);
	if (error)
		dev_err(&serio->dev,
			"device_add() failed for %s (%s), error: %d\n",
			serio->phys, serio->name, error);
}

static void serio_destroy_port(struct serio *serio)
{
	struct serio *child;

	while ((child = serio_get_pending_child(serio)) != NULL) {
		serio_remove_pending_events(child);
		put_device(&child->dev);
	}

	if (serio->stop)
		serio->stop(serio);

	if (serio->parent) {
		serio_pause_rx(serio->parent);
		list_del_init(&serio->child_node);
		serio_continue_rx(serio->parent);
		serio->parent = NULL;
	}

	if (device_is_registered(&serio->dev))
		device_del(&serio->dev);

	list_del_init(&serio->node);
	serio_remove_pending_events(serio);
	put_device(&serio->dev);
}

static int serio_reconnect_port(struct serio *serio)
{
	int error = serio_reconnect_driver(serio);

	if (error) {
		serio_disconnect_port(serio);
		serio_find_driver(serio);
	}

	return error;
}

static void serio_reconnect_subtree(struct serio *root)
{
	struct serio *s = root;
	int error;

	do {
		error = serio_reconnect_port(s);
		if (!error) {
			if (!list_empty(&s->children)) {
				s = list_first_entry(&s->children,
						     struct serio, child_node);
				continue;
			}
		}

		while (s != root) {
			struct serio *parent = s->parent;

			if (!list_is_last(&s->child_node, &parent->children)) {
				s = list_entry(s->child_node.next,
					       struct serio, child_node);
				break;
			}

			s = parent;
		}
	} while (s != root);
}

static void serio_disconnect_port(struct serio *serio)
{
	struct serio *s = serio;

	while (!list_empty(&serio->children)) {

		
		while (!list_empty(&s->children))
			s = list_first_entry(&s->children,
					     struct serio, child_node);

		if (s != serio) {
			struct serio *parent = s->parent;

			device_release_driver(&s->dev);
			serio_destroy_port(s);

			s = parent;
		}
	}

	device_release_driver(&serio->dev);
}

void serio_rescan(struct serio *serio)
{
	serio_queue_event(serio, NULL, SERIO_RESCAN_PORT);
}
EXPORT_SYMBOL(serio_rescan);

void serio_reconnect(struct serio *serio)
{
	serio_queue_event(serio, NULL, SERIO_RECONNECT_SUBTREE);
}
EXPORT_SYMBOL(serio_reconnect);

void __serio_register_port(struct serio *serio, struct module *owner)
{
	serio_init_port(serio);
	serio_queue_event(serio, owner, SERIO_REGISTER_PORT);
}
EXPORT_SYMBOL(__serio_register_port);

void serio_unregister_port(struct serio *serio)
{
	mutex_lock(&serio_mutex);
	serio_disconnect_port(serio);
	serio_destroy_port(serio);
	mutex_unlock(&serio_mutex);
}
EXPORT_SYMBOL(serio_unregister_port);

void serio_unregister_child_port(struct serio *serio)
{
	struct serio *s, *next;

	mutex_lock(&serio_mutex);
	list_for_each_entry_safe(s, next, &serio->children, child_node) {
		serio_disconnect_port(s);
		serio_destroy_port(s);
	}
	mutex_unlock(&serio_mutex);
}
EXPORT_SYMBOL(serio_unregister_child_port);



static ssize_t serio_driver_show_description(struct device_driver *drv, char *buf)
{
	struct serio_driver *driver = to_serio_driver(drv);
	return sprintf(buf, "%s\n", driver->description ? driver->description : "(none)");
}

static ssize_t serio_driver_show_bind_mode(struct device_driver *drv, char *buf)
{
	struct serio_driver *serio_drv = to_serio_driver(drv);
	return sprintf(buf, "%s\n", serio_drv->manual_bind ? "manual" : "auto");
}

static ssize_t serio_driver_set_bind_mode(struct device_driver *drv, const char *buf, size_t count)
{
	struct serio_driver *serio_drv = to_serio_driver(drv);
	int retval;

	retval = count;
	if (!strncmp(buf, "manual", count)) {
		serio_drv->manual_bind = true;
	} else if (!strncmp(buf, "auto", count)) {
		serio_drv->manual_bind = false;
	} else {
		retval = -EINVAL;
	}

	return retval;
}


static struct driver_attribute serio_driver_attrs[] = {
	__ATTR(description, S_IRUGO, serio_driver_show_description, NULL),
	__ATTR(bind_mode, S_IWUSR | S_IRUGO,
		serio_driver_show_bind_mode, serio_driver_set_bind_mode),
	__ATTR_NULL
};

static int serio_driver_probe(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);
	struct serio_driver *drv = to_serio_driver(dev->driver);

	return serio_connect_driver(serio, drv);
}

static int serio_driver_remove(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);

	serio_disconnect_driver(serio);
	return 0;
}

static void serio_cleanup(struct serio *serio)
{
	mutex_lock(&serio->drv_mutex);
	if (serio->drv && serio->drv->cleanup)
		serio->drv->cleanup(serio);
	mutex_unlock(&serio->drv_mutex);
}

static void serio_shutdown(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);

	serio_cleanup(serio);
}

static void serio_attach_driver(struct serio_driver *drv)
{
	int error;

	error = driver_attach(&drv->driver);
	if (error)
		pr_warning("driver_attach() failed for %s with error %d\n",
			   drv->driver.name, error);
}

int __serio_register_driver(struct serio_driver *drv, struct module *owner, const char *mod_name)
{
	bool manual_bind = drv->manual_bind;
	int error;

	drv->driver.bus = &serio_bus;
	drv->driver.owner = owner;
	drv->driver.mod_name = mod_name;

	drv->manual_bind = true;

	error = driver_register(&drv->driver);
	if (error) {
		pr_err("driver_register() failed for %s, error: %d\n",
			drv->driver.name, error);
		return error;
	}

	if (!manual_bind) {
		drv->manual_bind = false;
		error = serio_queue_event(drv, NULL, SERIO_ATTACH_DRIVER);
		if (error) {
			driver_unregister(&drv->driver);
			return error;
		}
	}

	return 0;
}
EXPORT_SYMBOL(__serio_register_driver);

void serio_unregister_driver(struct serio_driver *drv)
{
	struct serio *serio;

	mutex_lock(&serio_mutex);

	drv->manual_bind = true;	
	serio_remove_pending_events(drv);

start_over:
	list_for_each_entry(serio, &serio_list, node) {
		if (serio->drv == drv) {
			serio_disconnect_port(serio);
			serio_find_driver(serio);
			
			goto start_over;
		}
	}

	driver_unregister(&drv->driver);
	mutex_unlock(&serio_mutex);
}
EXPORT_SYMBOL(serio_unregister_driver);

static void serio_set_drv(struct serio *serio, struct serio_driver *drv)
{
	serio_pause_rx(serio);
	serio->drv = drv;
	serio_continue_rx(serio);
}

static int serio_bus_match(struct device *dev, struct device_driver *drv)
{
	struct serio *serio = to_serio_port(dev);
	struct serio_driver *serio_drv = to_serio_driver(drv);

	if (serio->manual_bind || serio_drv->manual_bind)
		return 0;

	return serio_match_port(serio_drv->id_table, serio);
}

#ifdef CONFIG_HOTPLUG

#define SERIO_ADD_UEVENT_VAR(fmt, val...)				\
	do {								\
		int err = add_uevent_var(env, fmt, val);		\
		if (err)						\
			return err;					\
	} while (0)

static int serio_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct serio *serio;

	if (!dev)
		return -ENODEV;

	serio = to_serio_port(dev);

	SERIO_ADD_UEVENT_VAR("SERIO_TYPE=%02x", serio->id.type);
	SERIO_ADD_UEVENT_VAR("SERIO_PROTO=%02x", serio->id.proto);
	SERIO_ADD_UEVENT_VAR("SERIO_ID=%02x", serio->id.id);
	SERIO_ADD_UEVENT_VAR("SERIO_EXTRA=%02x", serio->id.extra);
	SERIO_ADD_UEVENT_VAR("MODALIAS=serio:ty%02Xpr%02Xid%02Xex%02X",
				serio->id.type, serio->id.proto, serio->id.id, serio->id.extra);

	return 0;
}
#undef SERIO_ADD_UEVENT_VAR

#else

static int serio_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	return -ENODEV;
}

#endif 

#ifdef CONFIG_PM
static int serio_suspend(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);

	serio_cleanup(serio);

	return 0;
}

static int serio_resume(struct device *dev)
{
	struct serio *serio = to_serio_port(dev);

	serio_queue_event(serio, NULL, SERIO_RECONNECT_PORT);

	return 0;
}

static const struct dev_pm_ops serio_pm_ops = {
	.suspend	= serio_suspend,
	.resume		= serio_resume,
	.poweroff	= serio_suspend,
	.restore	= serio_resume,
};
#endif 

int serio_open(struct serio *serio, struct serio_driver *drv)
{
	serio_set_drv(serio, drv);

	if (serio->open && serio->open(serio)) {
		serio_set_drv(serio, NULL);
		return -1;
	}
	return 0;
}
EXPORT_SYMBOL(serio_open);

void serio_close(struct serio *serio)
{
	if (serio->close)
		serio->close(serio);

	serio_set_drv(serio, NULL);
}
EXPORT_SYMBOL(serio_close);

irqreturn_t serio_interrupt(struct serio *serio,
		unsigned char data, unsigned int dfl)
{
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;

	spin_lock_irqsave(&serio->lock, flags);

        if (likely(serio->drv)) {
                ret = serio->drv->interrupt(serio, data, dfl);
	} else if (!dfl && device_is_registered(&serio->dev)) {
		serio_rescan(serio);
		ret = IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&serio->lock, flags);

	return ret;
}
EXPORT_SYMBOL(serio_interrupt);

static struct bus_type serio_bus = {
	.name		= "serio",
	.dev_attrs	= serio_device_attrs,
	.drv_attrs	= serio_driver_attrs,
	.match		= serio_bus_match,
	.uevent		= serio_uevent,
	.probe		= serio_driver_probe,
	.remove		= serio_driver_remove,
	.shutdown	= serio_shutdown,
#ifdef CONFIG_PM
	.pm		= &serio_pm_ops,
#endif
};

static int __init serio_init(void)
{
	int error;

	error = bus_register(&serio_bus);
	if (error) {
		pr_err("Failed to register serio bus, error: %d\n", error);
		return error;
	}

	return 0;
}

static void __exit serio_exit(void)
{
	bus_unregister(&serio_bus);

	cancel_work_sync(&serio_event_work);
}

subsys_initcall(serio_init);
module_exit(serio_exit);
