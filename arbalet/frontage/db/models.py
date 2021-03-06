import datetime

# from server.extensions import db
from uuid import uuid4
from db.base import Base
from sqlalchemy import table, Column, Integer, String, DateTime, Boolean

def cln_str(s):
    if s:
        return s.replace(
            "'",
            "").replace(
            '\\',
            '').replace(
            '%',
            '').replace(
                ';',
            '')
    return ''

class DimensionsModel(Base):
    __tablename__ = 'dimensionsmodel'

    uniqid = Column(String(36), primary_key=True)
    rows = Column(Integer)
    cols = Column(Integer)

    def __init__(self):
        self.uniqid = str(uuid4())
        self.rows = 4
        self.cols = 19

    def __repr__(self):
        return '<dimensionsmodel %r (%r) (%r)>' % (
            self.uniqid, self.rows, self.cols)

class CellTableModel(Base):
    __tablename__ = 'celltablemodel'

    uniqid = Column(String(36), primary_key=True)
    X = Column(Integer)
    Y = Column(Integer)
    MacAddress = Column(String(60))

    def __init__(self, x, y, macAddress):
        self.uniqid = str(uuid4())
        self.X = x
        self.Y = y
        self.MacAddress = macAddress

    def __repr__(self):
        return '<celltablemodel %r (%r) (%r) (%r)>' % (
            self.uniqid, self.X, self.Y, self.MacAddress)

class FappModel(Base):
    __tablename__ = 'fapp'

    uniqid = Column(String(36), primary_key=True)
    name = Column(String(36), unique=True)
    is_scheduled = Column(Boolean)
    default_params = Column(String(4096))
    position = Column(Integer)

    def __init__(self, app_name, is_scheduled=False):
        self.uniqid = str(uuid4())
        self.position = 0
        self.name = app_name
        self.is_scheduled = is_scheduled
        self.default_params = '{}'

    def __repr__(self):
        return '<Fapp %r (%r) (%r) (%r)>' % (
            self.uniqid, self.is_scheduled, self.position, self.default_params)


class ConfigModel(Base):
    __tablename__ = 'configmodel'

    uniqid = Column(String(36), primary_key=True)

    forced_sunrise = Column(String(36))
    offset_sunrise = Column(Integer)

    forced_sunset = Column(String(36))
    offset_sunset = Column(Integer)
    state = Column(String(36))
    expires_delay = Column(Integer)
    default_app_lifetime = Column(Integer)

    admin_login = Column(String(36))
    admin_hash = Column(String(512))

    def __init__(self):
        self.uniqid = str(uuid4())
        self.forced_sunset = ""
        self.offset_sunset = 0
        self.state = 'scheduled'

        self.forced_sunrise = ""
        self.offset_sunrise = 0
        self.default_app_lifetime = 15 * 60

        self.expires_delay = 90

    def __repr__(self):
        return '<ConfigModel %r (%r) (%r)>' % (
            self.uniqid, self.expires_delay, self.forced_sunset)
