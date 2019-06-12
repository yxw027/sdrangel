///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2019 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QDataStream>

#include "util/simpleserializer.h"
#include "deviceuserarg.h"

QByteArray DeviceUserArg::serialize() const
{
    SimpleSerializer s(1);
    QByteArray data;
    QDataStream *stream = new QDataStream(&data, QIODevice::WriteOnly);
    *stream << m_argByDevice;
    s.writeBlob(1, data);
    return s.final();
}

bool DeviceUserArg::deserialize(const QByteArray& data)
{
    SimpleDeserializer d(data);

    if (!d.isValid()) {
        return false;
    }

    if(d.getVersion() == 1)
    {
        QByteArray data;

        d.readBlob(1, &data);
        QDataStream readStream(&data, QIODevice::ReadOnly);
        readStream >> m_argByDevice;

        return true;
    }
    else
    {
        return false;
    }
}

void DeviceUserArg::splitDeviceKey(const QString& key, QString& driver, int& sequence)
{
    QStringList elms = key.split('-');

    if (elms.size() > 0) {
        driver = elms[0];
    }

    if (elms.size() > 1)
    {
        bool ok;
        QString seqStr = elms[1];
        int seq = seqStr.toInt(&ok);

        if (ok) {
            sequence = seq;
        }
    }
}

void DeviceUserArg::composeDeviceKey(const QString& driver, int sequence, QString& key)
{
    QStringList strList;
    strList.append(driver);
    strList.append(QString::number(sequence));
    key = strList.join('-');
}
