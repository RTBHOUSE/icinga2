/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2014 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "remote/zone.h"
#include <boost/foreach.hpp>

using namespace icinga;

REGISTER_TYPE(Zone);

Zone::Ptr Zone::GetParent(void) const
{
	return Zone::GetByName(GetParentRaw());
}

std::set<Endpoint::Ptr> Zone::GetEndpoints(void) const
{
	std::set<Endpoint::Ptr> result;

	BOOST_FOREACH(const String& endpoint, GetEndpointsRaw())
		result.insert(Endpoint::GetByName(endpoint));

	return result;
}

bool Zone::CanAccessObject(const DynamicObject::Ptr& object)
{
	Zone::Ptr object_zone;

	if (dynamic_pointer_cast<Zone>(object))
		object_zone = static_pointer_cast<Zone>(object);
	else
		object_zone = Zone::GetByName(object->GetZone());

	if (!object_zone)
		object_zone = Zone::GetLocalZone();

	return object_zone->IsChildOf(GetSelf());
}

bool Zone::IsChildOf(const Zone::Ptr& zone)
{
	Zone::Ptr azone = GetSelf();

	while (azone) {
		if (azone == zone)
			return true;

		azone = azone->GetParent();
	}

	return false;
}

Zone::Ptr Zone::GetLocalZone(void)
{
	return Endpoint::GetLocalEndpoint()->GetZone();
}
