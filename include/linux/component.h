#ifndef COMPONENT_H
#define COMPONENT_H

#include <linux/stddef.h>

struct device;

/**
 * struct component_ops - callbacks for the components
 * @bind:	callback called when component_bind_all() is called
 * @unbind:	callback called when component_unbind_all() is called
 *
 * Struct used to pass to the component system the callbacks to bind and unbind
 * the component.
 */
struct component_ops {
	int (*bind)(struct device *comp, struct device *master,
		    void *master_data);
	void (*unbind)(struct device *comp, struct device *master,
		       void *master_data);
};

/**
 * component_add - add dev as a component in the component system
 * @dev:	The component device
 * @ops:	The struct component_ops
 *
 * Add a struct device as a component in the component system with the
 * corresponding bind/unbind callbacks.
 */
int component_add(struct device *dev, const struct component_ops *ops);

/**
 * component_del - remove a component from the component system
 * @dev:	The component device
 * @ops:	The struct component_ops previously used in component_add()
 *
 * Remove a struct device from the component system.
 */
void component_del(struct device *, const struct component_ops *);

/**
 * component_bind_all - bind all components from a given master
 * @master:		The master device
 * @master_data:	void pointer used as a parameter in struct component_ops
 *			bind() callback
 *
 * Trigger the call to bind() from struct component_ops for all components
 * associated with the master.
 * All components must have been found before calling the function. Usually it
 * should be called inside bind() from struct component_master_ops
 */
int component_bind_all(struct device *master, void *master_data);

/**
 * component_unbind_all - unbind all components from a given master
 * @master:		The master device
 * @master_data:	void pointer used as a parameter in struct component_ops
 *			unbind() callback
 *
 * Trigger the call to unbind() from struct component_ops for all components
 * associated with the master.
 */
void component_unbind_all(struct device *master, void *master_data);

/**
 * struct component_master_ops - callbacks for the master
 * @bind:	callback called when all the master's components are found
 * @unbind:	callback called when taking down the master. The master can be
 *		taken down if explicitly deleted or if one of its components is
 *		deleted from the component system.
 *
 * Struct used to pass to the component system the callbacks to bind and unbind
 * the master.
 */
struct component_master_ops {
	int (*bind)(struct device *master);
	void (*unbind)(struct device *master);
};

/**
 * component_master_del - remove a master from the component system
 * @dev:	The master device
 * @ops:	The struct component_master_ops previously used in
 *		component_master_add_with_match()
 *
 * Remove a struct device previously added as a master device from the component
 * system.
 */
void component_master_del(struct device *dev,
			  const struct component_master_ops *ops);

/**
 * struct component_match - match items holder
 *
 * This is an internal struct used to hold the match items created with
 * component_match_add* before adding the master to the component system with
 * component_master_add_with_match()
 */
struct component_match;

/**
 * component_master_add_with_match - add dev as a master in the component system
 * @dev:	The master device
 * @ops:	The struct component_master_ops
 * @match:	The struct component_match previously created with
 *		component_macth_add*()
 *
 * Add a struct device in the component system as a master device with the
 * corresponding bind/unbind callbacks and the list of match items.
 * Each match item correspond to a component. When all the corresponding
 * components are found, ops->bind() is called.
 * If the master is bound and one of the components is removed, then
 * ops->unbind() is called.
 */
int component_master_add_with_match(struct device *master,
				    const struct component_master_ops *ops,
				    struct component_match *match);

/**
 * component_match_add_release - associate a match item to the master
 * @master:	The master device
 * @matchptr:	The pointer to the match item array. If NULL then the array will
 *		be allocated.
 * @release:	Function called when the master releases the match item
 * @compare:	Function called to check if a component correspond to the match
 *		item
 * @cb_data:	Void pointer used in release and compare callbacks
 *
 * Add a new match item associated with the master device.
 * Each match item correspond to a component to be associated with this master.
 * When the master is added in the component system by
 * component_master_add_with_match(), the system will try to find the
 * appropriate component by calling compare(). If compare succeeds, then the
 * component is associated with the given match item.
 */
void component_match_add_release(struct device *master,
			struct component_match **matchptr,
			void (*release)(struct device *master, void *cb_data),
			int (*compare)(struct device *comp, void *cb_data),
			void *cb_data);

/**
 * component_match_add - associate a match item to the master
 * @master:	The master device
 * @matchptr:	The pointer to the match item array. If NULL then the array will
 *		be allocated.
 * @compare:	Function called to check if a component correspond to the match
 *		item
 * @compare_data: Void pointer used in release and compare callbacks
 *
 * Same as component_match_add_release() but with release argument as NULL
 */
static inline void component_match_add(struct device *master,
			struct component_match **matchptr,
			int (*compare)(struct device *comp, void *compare_data),
			void *compare_data)
{
	component_match_add_release(master, matchptr, NULL, compare,
				    compare_data);
}

#endif
